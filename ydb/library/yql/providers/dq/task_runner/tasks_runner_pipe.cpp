#include "tasks_runner_pipe.h"

#include <ydb/library/yql/dq/runtime/dq_input_channel.h>
#include <ydb/library/yql/dq/runtime/dq_output_channel.h>
#include <ydb/library/yql/minikql/invoke_builtins/mkql_builtins.h>
#include <ydb/library/yql/minikql/mkql_node_serialization.h>
#include <ydb/library/yql/minikql/mkql_node_cast.h>
#include <ydb/library/yql/minikql/mkql_program_builder.h>
#include <ydb/library/yql/minikql/aligned_page_pool.h>
#include <ydb/library/yql/utils/log/log.h>
#include <ydb/library/yql/utils/backtrace/backtrace.h>
#include <ydb/library/yql/utils/yql_panic.h>

#include <ydb/library/yql/providers/dq/common/yql_dq_settings.h>

#include <library/cpp/yson/node/node.h>
#include <library/cpp/yson/node/node_io.h>
#include <library/cpp/svnversion/svnversion.h>

#include <library/cpp/threading/task_scheduler/task_scheduler.h>

#include <util/system/shellcommand.h>
#include <util/system/thread.h>
#include <util/system/fs.h>
#include <util/stream/file.h>
#include <util/stream/pipe.h>
#include <util/generic/size_literals.h>
#include <util/string/cast.h>

namespace NYql::NTaskRunnerProxy {

const TString WorkingDirectoryParamName = "TaskRunnerProxy.WorkingDirectory";
const TString WorkingDirectoryDontInitParamName = "TaskRunnerProxy.WorkingDirectoryDontInit";
const TString UseMetaParamName = "TaskRunnerProxy.UseMeta"; // COMPAT(aozeritsky)

using namespace NKikimr;
using namespace NDq;

#ifndef _win_
extern "C" int fork(void);
extern "C" int dup2(int oldfd, int newfd);
extern "C" int close(int);
extern "C" int pipe(int pipefd[2]);
extern "C" int kill(int pid, int sig);
extern "C" int waitpid(int pid, int* status, int options);
#endif

template<typename T>
void FromProto(TDqOutputChannelStats* s, const T& f)
{
    s->ChannelId = f.GetChannelId();
    s->Chunks = f.GetChunks();
    s->Bytes = f.GetBytes();
    s->RowsIn = f.GetRowsIn();
    s->RowsOut = f.GetRowsOut();
    s->MaxMemoryUsage = f.GetMaxMemoryUsage();
    //s->StartTs = TInstant::MilliSeconds(f.GetStartTs());
    //s->FinishTs = TInstant::MilliSeconds(f.GetFinishTs());
}

template<typename T>
void FromProto(TDqInputChannelStats* s, const T& f)
{
    s->ChannelId = f.GetChannelId();
    s->Chunks = f.GetChunks();
    s->Bytes = f.GetBytes();
    s->RowsIn = f.GetRowsIn();
    s->RowsOut = f.GetRowsOut();
    s->MaxMemoryUsage = f.GetMaxMemoryUsage();
    //s->StartTs = TInstant::MilliSeconds(f.GetStartTs());
    //s->FinishTs = TInstant::MilliSeconds(f.GetFinishTs());
    s->DeserializationTime = TDuration::MicroSeconds(f.GetDeserializationTimeUs());
}

template<typename T>
void FromProto(TDqSourceStats* s, const T& f)
{
    s->InputIndex = f.GetInputIndex();
    s->Chunks = f.GetChunks();
    s->Bytes = f.GetBytes();
    s->RowsIn = f.GetRowsIn();
    s->RowsOut = f.GetRowsOut();
    s->MaxMemoryUsage = f.GetMaxMemoryUsage();
    //s->StartTs = TInstant::MilliSeconds(f.GetStartTs());
    //s->FinishTs = TInstant::MilliSeconds(f.GetFinishTs());
}

template<typename T>
void FromProto(TDqSinkStats* s, const T& f)
{
    s->Chunks = f.GetChunks();
    s->Bytes = f.GetBytes();
    s->RowsIn = f.GetRowsIn();
    s->RowsOut = f.GetRowsOut();
    s->MaxMemoryUsage = f.GetMaxMemoryUsage();
}

template<typename T>
void FromProto(TDqTaskRunnerStats* s, const T& f)
{
    //s->StartTs = TInstant::MilliSeconds(f.GetStartTs());
    //s->FinishTs = TInstant::MilliSeconds(f.GetFinishTs());
    s->BuildCpuTime = TDuration::MicroSeconds(f.GetBuildCpuTimeUs());
    s->ComputeCpuTime = TDuration::MicroSeconds(f.GetComputeCpuTimeUs());
    s->RunStatusTimeMetrics.Load(ERunStatus::PendingInput, TDuration::MicroSeconds(f.GetPendingInputTimeUs()));
    s->RunStatusTimeMetrics.Load(ERunStatus::PendingOutput, TDuration::MicroSeconds(f.GetPendingOutputTimeUs()));
    s->RunStatusTimeMetrics.Load(ERunStatus::Finished, TDuration::MicroSeconds(f.GetFinishTimeUs()));
    //s->TotalTime = TDuration::MilliSeconds(f.GetTotalTime());
    s->WaitTime = TDuration::MicroSeconds(f.GetWaitTimeUs());
    s->WaitOutputTime = TDuration::MicroSeconds(f.GetWaitOutputTimeUs());

    //s->MkqlTotalNodes = f.GetMkqlTotalNodes();
    //s->MkqlCodegenFunctions = f.GetMkqlCodegenFunctions();
    //s->CodeGenTotalInstructions = f.GetCodeGenTotalInstructions();
    //s->CodeGenTotalFunctions = f.GetCodeGenTotalFunctions();
    //s->CodeGenFullTime = f.GetCodeGenFullTime();
    //s->CodeGenFinalizeTime = f.GetCodeGenFinalizeTime();
    //s->CodeGenModulePassTime = f.GetCodeGenModulePassTime();

    for (const auto& input : f.GetInputChannels()) {
        FromProto(const_cast<TDqInputChannelStats*>(s->InputChannels[input.GetChannelId()]), input);
    }

    for (const auto& output : f.GetOutputChannels()) {
        FromProto(const_cast<TDqOutputChannelStats*>(s->OutputChannels[output.GetChannelId()]), output);
    }
}

class TChildProcess: private TNonCopyable {
public:
    TChildProcess(const TString& exeName, const TVector<TString>& args, const THashMap<TString, TString>& env, const TString& workDir)
        : ExeName(exeName)
        , Args(args)
        , Env(env)
        , WorkDir(workDir)
    {
        NFs::MakeDirectory(WorkDir);
    }

    virtual ~TChildProcess() {
        try {
            NFs::RemoveRecursive(WorkDir);
        } catch (...) { }
    }

    virtual TString ExternalWorkDir() {
        return WorkDir;
    }

    virtual TString InternalWorkDir() {
        return WorkDir;
    }

    void Run()
    {
#ifdef _win_
        Y_VERIFY(false);
#else
        int input[2];
        int output[2];
        int error[2];

        Y_VERIFY(pipe(input) == 0);
        Y_VERIFY(pipe(output) == 0);
        Y_VERIFY(pipe(error) == 0);

        Pid = fork();
        Y_VERIFY(Pid >= 0);

        if (Pid == 0) {
            try {
                close(input[1]);
                close(output[0]);
                close(error[0]);

                if (dup2(input[0], 0) == -1) {
                    ythrow TSystemError() << "Cannot dup2 input";
                } // stdin
                if (dup2(output[1], 1) == -1) {
                    ythrow TSystemError() << "Cannot dup2 output";
                } // stdout
                if  (dup2(error[1], 2) == -1) {
                    ythrow TSystemError() << "Cannot dup2 error";
                } // stderr

                Exec();
            } catch (const yexception& ex) {
                Cerr << ex.what() << Endl;
            }

            _exit(127);
        }

        close(input[0]);
        close(output[1]);
        close(error[1]);

        Stdin = MakeHolder<TPipedOutput>(input[1]);
        Stdout = MakeHolder<TPipedInput>(output[0]);
        Stderr = MakeHolder<TPipedInput>(error[0]);
#endif
    }

    virtual void Kill() {
#ifndef _win_
        kill(Pid, 9);
#endif
    }

    bool IsAlive() {
#ifdef _win_
        return true;
#else
        int status;
        YQL_LOG(DEBUG) << "Check Pid " << Pid;
        return waitpid(Pid, &status, WNOHANG) <= 0;
#endif
    }

    int Wait(TDuration timeout = TDuration::Seconds(5)) {
        int status;
        int ret;
#ifndef _win_
        TInstant start = TInstant::Now();
        while ((ret = waitpid(Pid, &status, WNOHANG)) == 0 && TInstant::Now() - start < timeout) {
            Sleep(TDuration::MilliSeconds(10));
        }
        if (ret <= 0) {
            kill(Pid, 9);
            waitpid(Pid, &status, 0);
        }
#endif
        return status;
    }

    IOutputStream& GetStdin() {
        return *Stdin;
    }

    IInputStream& GetStdout() {
        return *Stdout;
    }

    IInputStream& GetStderr() {
        return *Stderr;
    }

protected:
    TString ExeName;
    const TVector<TString> Args;
    const THashMap<TString, TString> Env;
    const TString WorkDir;

    THolder<TPipedOutput> Stdin;
    THolder<TPipedInput> Stdout;
    THolder<TPipedInput> Stderr;

    int Pid = -1;

    virtual void Exec() {
        char ** args;
        args = new char*[Args.size() + 1];
        ui32 i;
        for (i = 0; i < Args.size(); ++i) {
            args[i] = (char*) Args[i].c_str();
        }
        args[i] = nullptr;

        for (i = 3; i < 32768; ++i) {
            close(i);
        }

        char** env;
        env = new char*[Env.size() + 1];
        i = 0;
        for (const auto& [k, v] : Env) {
            env[i++] = (char*)(k + "=" + v).c_str();
        }
        env[i++] = nullptr;

        if (!WorkDir.empty()) {
            NFs::SetCurrentWorkingDirectory(WorkDir);
        }

        if (execve(ExeName.c_str(), (char*const*)args, (char*const*)env) == -1) {
            ythrow TSystemError() << "Cannot execl";
        }
    }
};

/*______________________________________________________________________________________________*/

struct TProcessHolder {
    TProcessHolder()
        : Watcher(MakeHolder<TThread>([this] () { Watch(); }))
    {
        Running.test_and_set();
        Watcher->Start();
    }

    ~TProcessHolder()
    {
        Running.clear();
        Watcher->Join();
    }

    i64 Size() {
        TGuard<TMutex> lock(Mutex);
        return Processes.size();
    }

    void Put(const TString& key, THolder<TChildProcess>process) {
        TGuard<TMutex> lock(Mutex);
        Processes.emplace_back(key, std::move(process));
    }

    THolder<TChildProcess> Acquire(const TString& key, TList<THolder<TChildProcess>>* stopList) {
        TGuard<TMutex> lock(Mutex);
        THolder<TChildProcess> result;
        while (!Processes.empty()) {
            auto first = std::move(Processes.front());
            Processes.pop_front();
            if (first.first == key) {
                result = std::move(first.second);
                break;
            }
            stopList->push_back(std::move(first.second));
        }
        return result;
    }

    void Watch() {
        while (Running.test()) {
            TList<THolder<TChildProcess>> stopList;
            {
                TGuard<TMutex> lock(Mutex);
                auto it = Processes.begin();
                while (it != Processes.end()) {
                    if (!it->second->IsAlive()) {
                        YQL_LOG(DEBUG) << "Remove dead process";
                        stopList.emplace_back(std::move(it->second));
                        it = Processes.erase(it);
                    } else {
                        ++it;
                    }
                }
            }

            for (const auto& job : stopList) {
                job->Kill();
                job->Wait(TDuration::Seconds(1));
            }
            Sleep(TDuration::MilliSeconds(1000));
        }
    }

    THolder<TThread> Watcher;
    std::atomic_flag Running;

    TMutex Mutex;
    TList<std::pair<TString, THolder<TChildProcess>>> Processes;
};

struct TPortoSettings {
    bool Enable;
    ui64 MemoryLimit;
    TString Layer;
    TString ContainerNamePrefix;

    bool operator == (const TPortoSettings& o) const {
        return Enable == o.Enable && MemoryLimit == o.MemoryLimit && Layer == o.Layer;
    }

    bool operator != (const TPortoSettings& o) const {
        return ! operator == (o);
    }

    TString ToString() const {
        return TStringBuilder() << Enable << "," << MemoryLimit << "," << Layer;
    }
};

class TPortoProcess: public TChildProcess
{
public:
    TPortoProcess(const TString& portoCtl, const TString& exeName, const TVector<TString>& args, const THashMap<TString, TString>& env, const TString& workDir, const TPortoSettings& portoSettings)
        : TChildProcess(exeName, args, env, workDir)
        , PortoCtl(portoCtl)
        , PortoLayer(portoSettings.Layer)
        , MemoryLimit(portoSettings.MemoryLimit)
        , ContainerName(WorkDir.substr(WorkDir.rfind("/")+1))
        , InternalWorkDir_("mnt/work")
        , InternalExeDir("usr/local/bin")
        , TmpDir("TmpDir" + ContainerName)
    {
        NFs::MakeDirectory(TmpDir);
        auto pos = ExeName.rfind("/");
        TString name = ExeName.substr(pos+1);

        if (portoSettings.ContainerNamePrefix) {
            ContainerName = portoSettings.ContainerNamePrefix + "/" + ContainerName;
        }

        YQL_LOG(DEBUG) << "HardLink " << ExeName << "'" << WorkDir + "/" + name << "'";
        if (NFs::HardLink(ExeName, WorkDir + "/" + name)) {
            ExeName = WorkDir + "/" + name;
        } else {
            YQL_LOG(DEBUG) << "HardLink Failed " << ExeName << "'" << WorkDir + "/" + name << "'";
        }
    }

    TString InternalWorkDir() override {
        return InternalWorkDir_;
    }

    ~TPortoProcess() {
        try {
            NFs::RemoveRecursive(TmpDir);
        } catch (...) { }
    }

private:
    void Kill() override {
        try {
            // see YQL-13760
            TShellCommand cmd(PortoCtl, {"set", ContainerName, "anon_limit", "0"});
            cmd.Run().Wait();
        } catch (...) {
            YQL_LOG(DEBUG) << "Cannot set anon_limit: " << CurrentExceptionMessage();
        }
        try {
            TShellCommand cmd(PortoCtl, {"destroy", ContainerName});
            cmd.Run().Wait();
        } catch (...) {
            YQL_LOG(DEBUG) << "Cannot destroy: " << CurrentExceptionMessage();
        }
        TChildProcess::Kill();
    }

    void Exec() override {
        auto pos = ExeName.rfind("/");
        TString exeDir = ExeName.substr(0, pos);
        TString exeName = ExeName.substr(pos+1);

        TString command = InternalExeDir +"/"+ exeName + " ";
        for (ui64 i = 1; i < Args.size(); ++i) {
            command += Args[i] + " ";
        }

        TString caps = "CHOWN;DAC_OVERRIDE;FOWNER;KILL;SETPCAP;IPC_LOCK;SYS_CHROOT;SYS_PTRACE;MKNOD;AUDIT_WRITE;SETFCAP";
        TString env;
        for (const auto& [k, v] : Env) {
            env += k + "=" + v + ";";
        }
        env += "TMPDIR=/tmp;";

        TString bind;
        //bind += WorkDir + " " + InternalWorkDir() + " ro;";
        bind += WorkDir + " " + InternalWorkDir() + " rw;"; // see YQL-11392
        bind += exeDir + " " + InternalExeDir + " ro;";
        bind += TmpDir + " /tmp rw;";

        TVector<TString> vargs = {
            "portoctl",
            "exec",
            "-L",
            PortoLayer.empty() ? "/" : PortoLayer,
            ContainerName,
            "command=" + command,
            "capabilities=" + caps,
            "env=" + env,
            "bind=" + bind,
            "root_readonly=true",
            "weak=false",
            "cpu_policy=idle",
            "anon_limit=" + ToString(MemoryLimit)
        };

        char ** args;
        args = new char*[vargs.size() + 1];
        ui32 i;
        for (i = 0; i < vargs.size(); ++i) {
            args[i] = (char*) vargs[i].c_str();
        }
        args[i] = nullptr;

        for (i = 3; i < 32768; ++i) {
            close(i);
        }

        if (execvp(PortoCtl.c_str(), (char*const*)args) == -1) {
            ythrow TSystemError() << "Cannot execl";
        }
    }

    const TString PortoCtl;
    const TString PortoLayer;
    const ui64 MemoryLimit;
    TString ContainerName;

    const TString InternalWorkDir_;
    const TString InternalExeDir;
    const TString TmpDir;
};

/*______________________________________________________________________________________________*/


class IPipeTaskRunner: public ITaskRunner {
public:
    virtual IOutputStream& GetOutput() = 0;
    virtual IInputStream& GetInput() = 0;
    [[noreturn]]
    virtual void RaiseException() = 0;
};

/*______________________________________________________________________________________________*/

class TInputChannel : public IInputChannel {
public:
    TInputChannel(const ITaskRunner::TPtr& taskRunner, ui64 taskId, ui64 channelId, IInputStream& input, IOutputStream& output)
        : TaskId(taskId)
        , ChannelId(channelId)
        , Input(input)
        , Output(output)
        , ProtocolVersion(taskRunner->GetProtocolVersion())
    { }

    i64 GetFreeSpace() override {
        if (ProtocolVersion <= 1) {
            return std::numeric_limits<i64>::max();
        }

        NDqProto::TCommandHeader header;
        header.SetVersion(2);
        header.SetCommand(NDqProto::TCommandHeader::GET_FREE_SPACE);
        header.SetTaskId(TaskId);
        header.SetChannelId(ChannelId);
        header.Save(&Output);

        NDqProto::TGetFreeSpaceResponse response;
        response.Load(&Input);
        return response.GetFreeSpace();
    }

    void Push(NDqProto::TData&& data) override {
        NDqProto::TCommandHeader header;
        header.SetVersion(1);
        header.SetCommand(NDqProto::TCommandHeader::PUSH);
        header.SetTaskId(TaskId);
        header.SetChannelId(ChannelId);
        header.Save(&Output);
        data.Save(&Output);
    }

    void Finish() override {
        NDqProto::TCommandHeader header;
        header.SetVersion(1);
        header.SetCommand(NDqProto::TCommandHeader::FINISH);
        header.SetTaskId(TaskId);
        header.SetChannelId(ChannelId);
        header.Save(&Output);
    }

private:
    ui64 TaskId;
    ui64 ChannelId;

    IInputStream& Input;
    IOutputStream& Output;

    TString SerializedInputType;

    i32 ProtocolVersion;
};

class TDqInputChannel: public IDqInputChannel {
public:
    TDqInputChannel(const IInputChannel::TPtr& channel, ui64 taskId, ui64 channelId, IPipeTaskRunner* taskRunner)
        : Delegate(channel)
        , TaskId(taskId)
        , ChannelId(channelId)
        , Stats(ChannelId)
        , TaskRunner(taskRunner)
        , Input(TaskRunner->GetInput())
        , Output(TaskRunner->GetOutput())
    { }

    ui64 GetChannelId() const override {
        return ChannelId;
    }

    i64 GetFreeSpace() const override {
        try {
            return Delegate->GetFreeSpace();
        } catch (...) {
            TaskRunner->RaiseException();
        }
    }

    ui64 GetStoredBytes() const override {
        try {
            NDqProto::TCommandHeader header;
            header.SetVersion(3);
            header.SetCommand(NDqProto::TCommandHeader::GET_STORED_BYTES);
            header.SetTaskId(TaskId);
            header.SetChannelId(ChannelId);
            header.Save(&Output);

            NDqProto::TGetStoredBytesResponse response;
            response.Load(&Input);

            return response.GetResult();
        } catch (...) {
            TaskRunner->RaiseException();
        }
    }

    bool Empty() const override {
        ythrow yexception() << "unimplemented";
    }

    void Push(NDqProto::TData&& data) override {
        try {
            return Delegate->Push(std::move(data));
        } catch (...) {
            TaskRunner->RaiseException();
        }
    }

    [[nodiscard]]
    bool Pop(NKikimr::NMiniKQL::TUnboxedValueVector& batch) override {
        Y_UNUSED(batch);
        ythrow yexception() << "unimplemented";
    }

    void Finish() override {
        try {
            Delegate->Finish();
        } catch (...) {
            TaskRunner->RaiseException();
        }
    }

    bool IsFinished() const override {
        ythrow yexception() << "unimplemented";
    }

    const TDqInputChannelStats* GetStats() const override {
        try {
            NDqProto::TCommandHeader header;
            header.SetVersion(3);
            header.SetCommand(NDqProto::TCommandHeader::GET_STATS_INPUT);
            header.SetTaskId(TaskId);
            header.SetChannelId(ChannelId);
            header.Save(&Output);

            NDqProto::TGetStatsInputResponse response;
            response.Load(&Input);

            FromProto(&Stats, response.GetStats());
            return &Stats;
        } catch (...) {
            TaskRunner->RaiseException();
        }
    }

    NKikimr::NMiniKQL::TType* GetInputType() const override {
        ythrow yexception() << "unimplemented";
    }

    void Pause() override {
        Y_FAIL("Checkpoints are not supported");
    }

    void Resume() override {
        Y_FAIL("Checkpoints are not supported");
    }

    bool IsPaused() const override {
        return false;
    }

private:
    IInputChannel::TPtr Delegate;
    ui64 TaskId;
    ui64 ChannelId;
    mutable TDqInputChannelStats Stats;

    IPipeTaskRunner* TaskRunner;
    IInputStream& Input;
    IOutputStream& Output;
};

class TDqSource: public IStringSource {
public:
    TDqSource(ui64 taskId, ui64 inputIndex, IPipeTaskRunner* taskRunner)
        : TaskId(taskId)
        , InputIndex(inputIndex)
        , Stats(inputIndex)
        , TaskRunner(taskRunner)
        , Input(TaskRunner->GetInput())
        , Output(TaskRunner->GetOutput())
    { }

    i64 GetFreeSpace() const override {
        NDqProto::TCommandHeader header;
        header.SetVersion(4);
        header.SetCommand(NDqProto::TCommandHeader::GET_FREE_SPACE_SOURCE);
        header.SetTaskId(TaskId);
        header.SetChannelId(InputIndex);
        header.Save(&Output);

        NDqProto::TGetFreeSpaceResponse response;
        response.Load(&Input);
        return response.GetFreeSpace();
    }

    ui64 GetStoredBytes() const override {
        NDqProto::TCommandHeader header;
        header.SetVersion(4);
        header.SetCommand(NDqProto::TCommandHeader::GET_STORED_BYTES_SOURCE);
        header.SetTaskId(TaskId);
        header.SetChannelId(InputIndex);
        header.Save(&Output);

        NDqProto::TGetStoredBytesResponse response;
        response.Load(&Input);

        return response.GetResult();
    }

    bool Empty() const override {
        ythrow yexception() << "unimplemented";
    }

    void PushString(TVector<TString>&& batch, i64 space) override {
        if (space > static_cast<i64>(256_MB)) {
            throw yexception() << "Too big batch " << space;
        }
        NDqProto::TSourcePushRequest data;
        data.SetSpace(space);
        data.SetChunks(batch.size());

        NDqProto::TCommandHeader header;
        header.SetVersion(4);
        header.SetCommand(NDqProto::TCommandHeader::PUSH_SOURCE);
        header.SetTaskId(TaskId);
        header.SetChannelId(InputIndex);
        header.Save(&Output);
        data.Save(&Output);

        i64 partSize = 32_MB;
        for (auto& b : batch) {
            NDqProto::TSourcePushChunk chunk;
            i64 parts = (b.size() + partSize - 1) /  partSize;
            chunk.SetParts(parts);
            if (parts == 1) {
                chunk.SetString(std::move(b));
                chunk.Save(&Output);
            } else {
                chunk.Save(&Output);
                for (i64 i = 0; i < parts; i++) {
                    NDqProto::TSourcePushPart part;
                    part.SetString(b.substr(i*partSize, partSize));
                    part.Save(&Output);
                }
            }
        }
    }

    void Push(NKikimr::NMiniKQL::TUnboxedValueVector&& batch, i64 space) override {
        auto inputType = GetInputType();
        NDqProto::TSourcePushRequest data;
        TDqDataSerializer dataSerializer(TaskRunner->GetTypeEnv(), TaskRunner->GetHolderFactory(), NDqProto::DATA_TRANSPORT_UV_PICKLE_1_0); 
        *data.MutableData() = dataSerializer.Serialize(batch, static_cast<NKikimr::NMiniKQL::TType*>(inputType));
        data.SetSpace(space);

        NDqProto::TCommandHeader header;
        header.SetVersion(4);
        header.SetCommand(NDqProto::TCommandHeader::PUSH_SOURCE);
        header.SetTaskId(TaskId);
        header.SetChannelId(InputIndex);
        header.Save(&Output);
        data.Save(&Output);
    }

    [[nodiscard]]
    bool Pop(NKikimr::NMiniKQL::TUnboxedValueVector& batch) override {
        Y_UNUSED(batch);
        ythrow yexception() << "unimplemented";
    }

    void Finish() override {
        NDqProto::TCommandHeader header;
        header.SetVersion(4);
        header.SetCommand(NDqProto::TCommandHeader::FINISH_SOURCE);
        header.SetTaskId(TaskId);
        header.SetChannelId(InputIndex);
        header.Save(&Output);
    }

    bool IsFinished() const override {
        ythrow yexception() << "unimplemented";
    }

    ui64 GetInputIndex() const override {
        return InputIndex;
    }

    const TDqSourceStats* GetStats() const override {
        try {
            NDqProto::TCommandHeader header;
            header.SetVersion(4);
            header.SetCommand(NDqProto::TCommandHeader::GET_STATS_SOURCE);
            header.SetTaskId(TaskId);
            header.SetChannelId(InputIndex);
            header.Save(&Output);

            NDqProto::TGetStatsSourceResponse response;
            response.Load(&Input);

            FromProto(&Stats, response.GetStats());
            return &Stats;
        } catch (...) {
            TaskRunner->RaiseException();
        }
    }

    NKikimr::NMiniKQL::TType* GetInputType() const override {
        if (InputType) {
            return InputType;
        }

        NDqProto::TCommandHeader header;
        header.SetVersion(4);
        header.SetCommand(NDqProto::TCommandHeader::GET_SOURCE_TYPE);
        header.SetTaskId(TaskId);
        header.SetChannelId(InputIndex);
        header.Save(&Output);

        NDqProto::TGetTypeResponse response;
        response.Load(&Input);

        InputType = static_cast<NKikimr::NMiniKQL::TType*>(NKikimr::NMiniKQL::DeserializeNode(response.GetResult(), TaskRunner->GetTypeEnv()));
        return InputType;
    }

    void Pause() override {
        Y_FAIL("Checkpoints are not supported");
    }

    void Resume() override {
        Y_FAIL("Checkpoints are not supported");
    }

    bool IsPaused() const override {
        return false;
    }

private:
    ui64 TaskId;
    ui64 InputIndex;
    mutable TDqSourceStats Stats;

    IPipeTaskRunner* TaskRunner;
    IInputStream& Input;
    IOutputStream& Output;
    mutable NKikimr::NMiniKQL::TType* InputType = nullptr;
};

/*______________________________________________________________________________________________*/

class TOutputChannel : public IOutputChannel {
public:
    TOutputChannel(ui64 taskId, ui64 channelId, IInputStream& input, IOutputStream& output)
        : TaskId(taskId)
        , ChannelId(channelId)
        , Input(input)
        , Output(output)
    { }

    [[nodiscard]]
    NDqProto::TPopResponse Pop(NDqProto::TData& data, ui64) override {
        NDqProto::TCommandHeader header;
        header.SetVersion(1);
        header.SetCommand(NDqProto::TCommandHeader::POP);
        header.SetTaskId(TaskId);
        header.SetChannelId(ChannelId);
        header.Save(&Output);

        NDqProto::TPopResponse response;
        response.Load(&Input);
        data = std::move(*response.MutableData());
        return response;
    }

    bool IsFinished() const override {
        if (Finished) {
            return true;
        }
        NDqProto::TCommandHeader header;
        header.SetVersion(1);
        header.SetCommand(NDqProto::TCommandHeader::IS_FINISHED);
        header.SetTaskId(TaskId);
        header.SetChannelId(ChannelId);
        header.Save(&Output);

        NDqProto::TIsFinishedResponse response;
        response.Load(&Input);
        Finished = response.GetResult();
        return Finished;
    }

private:
    mutable bool Finished = false;
    ui64 TaskId;
    ui64 ChannelId;

    IInputStream& Input;
    IOutputStream& Output;
};

class TDqOutputChannel: public IDqOutputChannel {
public:
    TDqOutputChannel(const IOutputChannel::TPtr& channel, ui64 taskId, ui64 channelId, IPipeTaskRunner* taskRunner)
        : Delegate(channel)
        , TaskId(taskId)
        , ChannelId(channelId)
        , Stats(ChannelId)
        , TaskRunner(taskRunner)
        , Input(TaskRunner->GetInput())
        , Output(TaskRunner->GetOutput())
    {
        Y_UNUSED(Input);
    }

    ui64 GetChannelId() const override {
        return ChannelId;
    }

    ui64 GetValuesCount(bool inMemoryOnly) const override {
        Y_UNUSED(inMemoryOnly);
        ythrow yexception() << "unimplemented";
    }

    // <| producer methods
    [[nodiscard]]
    bool IsFull() const override {
        ythrow yexception() << "unimplemented";
    };

    // can throw TDqChannelStorageException
    void Push(NUdf::TUnboxedValue&& value) override {
        Y_UNUSED(value);
        ythrow yexception() << "unimplemented";
    }

    void Push(NDqProto::TCheckpoint&& checkpoint) override {
        Y_UNUSED(checkpoint);
        ythrow yexception() << "unimplemented";
    }

    void Finish() override {
        try {
            NDqProto::TCommandHeader header;
            header.SetVersion(3);
            header.SetCommand(NDqProto::TCommandHeader::FINISH_OUTPUT);
            header.SetTaskId(TaskId);
            header.SetChannelId(ChannelId);
            header.Save(&Output);
        } catch (...) {
            TaskRunner->RaiseException();
        }
    }
    // |>

    // <| consumer methods

    bool HasData() const override {
        ythrow yexception() << "unimplemented";
    }

    bool IsFinished() const override {
        try {
            return Delegate->IsFinished();
        } catch (...) {
            TaskRunner->RaiseException();
        }
    }
    // can throw TDqChannelStorageException
    [[nodiscard]]
    bool Pop(NDqProto::TData& data, ui64 bytes) override {
        try {
            auto response = Delegate->Pop(data, bytes);
            return response.GetResult();
        } catch (...) {
            TaskRunner->RaiseException();
        }
    }

    bool Pop(NDqProto::TCheckpoint&) override {
        return false;
    }

    // Only for data-queries
    // TODO: remove this method and create independent Data- and Stream-query implementations.
    //       Stream-query implementation should be without PopAll method.
    //       Data-query implementation should be one-shot for Pop (a-la PopAll) call and without ChannelStorage.
    // can throw TDqChannelStorageException
    [[nodiscard]]
    bool PopAll(NDqProto::TData& data) override {
        Y_UNUSED(data);
        ythrow yexception() << "unimplemented";
    }

    bool PopAll(NKikimr::NMiniKQL::TUnboxedValueVector& rows) override {
        Y_UNUSED(rows);
        ythrow yexception() << "unimplemented";
    }
    // |>

    ui64 Drop() override {
        try {
            NDqProto::TCommandHeader header;
            header.SetVersion(3);
            header.SetCommand(NDqProto::TCommandHeader::DROP_OUTPUT);
            header.SetTaskId(TaskId);
            header.SetChannelId(ChannelId);
            header.Save(&Output);

            NDqProto::TDropOutputResponse response;
            response.Load(&Input);

            return response.GetResult();
        } catch (...) {
            TaskRunner->RaiseException();
        }
    }

    NKikimr::NMiniKQL::TType* GetOutputType() const override {
        Y_FAIL("Unimplemented");
        return nullptr;
    }

    const TDqOutputChannelStats* GetStats() const override {
        try {
            NDqProto::TCommandHeader header;
            header.SetVersion(3);
            header.SetCommand(NDqProto::TCommandHeader::GET_STATS_OUTPUT);
            header.SetTaskId(TaskId);
            header.SetChannelId(ChannelId);
            header.Save(&Output);

            NDqProto::TGetStatsOutputResponse response;
            response.Load(&Input);

            FromProto(&Stats, response.GetStats());
            return &Stats;
        } catch (...) {
            TaskRunner->RaiseException();
        }
    }

    void Terminate() override {
        try {
            NDqProto::TCommandHeader header;
            header.SetVersion(3);
            header.SetCommand(NDqProto::TCommandHeader::TERMINATE_OUTPUT);
            header.SetTaskId(TaskId);
            header.SetChannelId(ChannelId);
            header.Save(&Output);
        } catch (...) {
            TaskRunner->RaiseException();
        }
    }

private:
    IOutputChannel::TPtr Delegate;
    ui64 TaskId;
    ui64 ChannelId;
    mutable TDqOutputChannelStats Stats;
    IPipeTaskRunner* TaskRunner;
    IInputStream& Input;
    IOutputStream& Output;
};

class TDqSink : public IStringSink {
public:
    TDqSink(ui64 taskId, ui64 index, IPipeTaskRunner* taskRunner)
        : TaskId(taskId)
        , Index(index)
        , TaskRunner(taskRunner)
        , Input(TaskRunner->GetInput())
        , Output(TaskRunner->GetOutput())
        , Stats(Index)
    { }

    ui64 GetOutputIndex() const override {
        return Index;
    }

    ui64 PopString(TVector<TString>& batch, ui64 bytes) override {
        try {
            NDqProto::TCommandHeader header;
            header.SetVersion(5);
            header.SetCommand(NDqProto::TCommandHeader::SINK_POP);
            header.SetTaskId(TaskId);
            header.SetChannelId(Index);
            header.Save(&Output);

            NDqProto::TSinkPopRequest request;
            request.SetBytes(bytes);
            request.SetRaw(true);
            header.Save(&Output);

            NDqProto::TSinkPopResponse response;
            response.Load(&Input);

            for (auto& row : response.GetString()) {
                batch.emplace_back(std::move(row));
            }

            return response.GetBytes();
        } catch (...) {
            TaskRunner->RaiseException();
        }
    }

    ui64 Pop(NKikimr::NMiniKQL::TUnboxedValueVector& batch, ui64 bytes) override {
        try {
            NDqProto::TCommandHeader header;
            header.SetVersion(5);
            header.SetCommand(NDqProto::TCommandHeader::SINK_POP);
            header.SetTaskId(TaskId);
            header.SetChannelId(Index);
            header.Save(&Output);

            NDqProto::TSinkPopRequest request;
            request.SetBytes(bytes);
            header.Save(&Output);

            NDqProto::TSinkPopResponse response;
            response.Load(&Input);

            TDqDataSerializer dataSerializer(
                TaskRunner->GetTypeEnv(),
                TaskRunner->GetHolderFactory(),
                NDqProto::DATA_TRANSPORT_UV_PICKLE_1_0);

            dataSerializer.Deserialize(
                response.GetData(),
                GetOutputType(),
                batch);

            return response.GetBytes();
        } catch (...) {
            TaskRunner->RaiseException();
        }
    }

    bool IsFinished() const override {
        try {
            NDqProto::TCommandHeader header;
            header.SetVersion(5);
            header.SetCommand(NDqProto::TCommandHeader::SINK_IS_FINISHED);
            header.SetTaskId(TaskId);
            header.SetChannelId(Index);
            header.Save(&Output);

            NDqProto::TIsFinishedResponse response;
            response.Load(&Input);

            return response.GetResult();
        } catch (...) {
            TaskRunner->RaiseException();
        }
    }

    NKikimr::NMiniKQL::TType* GetOutputType() const override {
        if (OutputType) {
            return OutputType;
        }

        NDqProto::TCommandHeader header;
        header.SetVersion(5);
        header.SetCommand(NDqProto::TCommandHeader::SINK_OUTPUT_TYPE);
        header.SetTaskId(TaskId);
        header.SetChannelId(Index);
        header.Save(&Output);

        NDqProto::TGetTypeResponse response;
        response.Load(&Input);

        OutputType = static_cast<NKikimr::NMiniKQL::TType*>(
            NKikimr::NMiniKQL::DeserializeNode(
                response.GetResult(),
                TaskRunner->GetTypeEnv()));
        return OutputType;
    }

    const TDqSinkStats* GetStats() const override {
        try {
            NDqProto::TCommandHeader header;
            header.SetVersion(5);
            header.SetCommand(NDqProto::TCommandHeader::SINK_STATS);
            header.SetTaskId(TaskId);
            header.SetChannelId(Index);
            header.Save(&Output);

            NDqProto::TSinkStatsResponse response;
            response.Load(&Input);

            FromProto(&Stats, response.GetStats());
            return &Stats;
        } catch (...) {
            TaskRunner->RaiseException();
        }
    }

    void Finish() override {
        Y_FAIL("Unimplemented");
    }

    bool Pop(NDqProto::TCheckpoint& checkpoint) override {
        Y_UNUSED(checkpoint);
        Y_FAIL("Checkpoints are not supported");
    }

    bool IsFull() const override {
        Y_FAIL("Unimplemented");
    }

    void Push(NUdf::TUnboxedValue&& value) override {
        Y_UNUSED(value);
        Y_FAIL("Unimplemented");
    }

    void Push(NDqProto::TCheckpoint&& checkpoint) override {
        Y_UNUSED(checkpoint);
        Y_FAIL("Checkpoints are not supported");
    }

    bool HasData() const override {
        Y_FAIL("Unimplemented");
    }

private:
    ui64 TaskId;
    ui64 Index;
    IPipeTaskRunner* TaskRunner;

    IInputStream& Input;
    IOutputStream& Output;

    mutable TDqSinkStats Stats;
    mutable NKikimr::NMiniKQL::TType* OutputType = nullptr;
};

/*______________________________________________________________________________________________*/

class TTaskRunner: public IPipeTaskRunner {
public:
    TTaskRunner(
        const NDqProto::TDqTask& task,
        THolder<TChildProcess>&& command,
        ui64 stageId,
        const TString& traceId)
        : TraceId(traceId)
        , Task(task)
        , Alloc(NKikimr::TAlignedPagePoolCounters(), true)
        , TypeEnv(Alloc)
        , MemInfo("TDqTaskRunnerProxy")
        , HolderFactory(Alloc.Ref(), MemInfo)
        , Running(true)
        , Command(std::move(command))
        , StderrReader(MakeHolder<TThread>([this] () { ReadStderr(); }))
        , Output(Command->GetStdin())
        , Input(Command->GetStdout())
        , TaskId(Task.GetId())
        , StageId(stageId)
    {
        Alloc.Release();
        StderrReader->Start();
        InitTaskMeta();
    }

    ~TTaskRunner() {
        Alloc.Acquire();
        Command->Kill();
        Command->Wait(TDuration::Seconds(0));
    }

    void ReadStderr() {
        auto& input = Command->GetStderr();
        char buf[1024];
        size_t size;
        while ((size = input.Read(buf, sizeof(buf))) > 0) {
            auto str = TString(buf, size);
            Stderr += str;
            YQL_LOG(DEBUG) << "stderr (" << StageId << " " << TraceId << " ) > `" << str << "'";
        }
        YQL_LOG(DEBUG) << "stderr (" << StageId << " " << TraceId << " ) finished";
    }

    ui64 GetTaskId() const override {
        return TaskId;
    }

    i32 GetProtocolVersion() override {
        if (ProtocolVersion < 0) {
            NDqProto::TCommandHeader header;
            header.SetVersion(1);
            header.SetCommand(NDqProto::TCommandHeader::VERSION);
            header.Save(&Output);

            NDqProto::TGetVersionResponse response;
            response.Load(&Input);

            ProtocolVersion = response.GetVersion();
        }

        return ProtocolVersion;
    }

    NYql::NDqProto::TPrepareResponse Prepare() override {
        NDqProto::TCommandHeader header;
        header.SetVersion(1);
        header.SetCommand(NDqProto::TCommandHeader::PREPARE);
        header.Save(&Output);

        NDqProto::TPrepareRequest request;
        *request.MutableTask() = Task;
        request.Save(&Output);

        NYql::NDqProto::TPrepareResponse ret;
        ret.Load(&Input);
        return ret;
    }

    NYql::NDqProto::TRunResponse Run() override {
        NDqProto::TCommandHeader header;
        header.SetVersion(1);
        header.SetCommand(NDqProto::TCommandHeader::RUN);
        header.SetTaskId(Task.GetId());
        header.Save(&Output);

        NDqProto::TRunResponse response;
        response.Load(&Input);
        return response;
    }

    IInputChannel::TPtr GetInputChannel(ui64 channelId) override {
        return new TInputChannel(this, Task.GetId(), channelId, Input, Output);
    }

    IOutputChannel::TPtr GetOutputChannel(ui64 channelId) override {
        return new TOutputChannel(Task.GetId(), channelId, Input, Output);
    }

    IDqSource::TPtr GetSource(ui64 index) override {
        return new TDqSource(Task.GetId(), index, this);
    }

    TDqSink::TPtr GetSink(ui64 index) override {
        return new TDqSink(Task.GetId(), index, this);
    }

    const NMiniKQL::TTypeEnvironment& GetTypeEnv() const override {
        return TypeEnv;
    }

    const NMiniKQL::THolderFactory& GetHolderFactory() const override {
        return HolderFactory;
    }

    const THashMap<TString, TString>& GetSecureParams() const override {
        return SecureParams;
    }

    const THashMap<TString, TString>& GetTaskParams() const override {
        return TaskParams;
    }

    TGuard<NKikimr::NMiniKQL::TScopedAlloc> BindAllocator(TMaybe<ui64> memoryLimit) override {
        auto guard = TypeEnv.BindAllocator();
        if (memoryLimit) {
            guard.GetMutex()->SetLimit(*memoryLimit);
        }
        return guard;
    }

    bool IsAllocatorAttached() override {
        return TypeEnv.GetAllocator().IsAttached();
    }

    void Kill() override {
        bool expected = true;
        if (!Running.compare_exchange_strong(expected, false)) {
            return;
        }

        Command->Kill();
        Command->Wait(TDuration::Seconds(0));
        StderrReader->Join();
    }

    TStatus GetStatus() override {
        bool expected = true;
        if (!Running.compare_exchange_strong(expected, false)) {
            return {Code, Stderr};
        }

        try {
            NDqProto::TCommandHeader header;
            header.SetVersion(1);
            header.SetCommand(NDqProto::TCommandHeader::STOP);
            header.Save(&Output);
        } catch (...) { }
        Code = Command->Wait();
        StderrReader->Join();
        return {Code, Stderr};
    }

    [[noreturn]]
    void RaiseException() override {
        auto status = GetStatus();
        TString message;
        message += "ExitCode: " + ToString(status.ExitCode) + "\n";
        message += "Stderr:\n" + status.Stderr + "\n";
        ythrow yexception() << message;
    }

    IOutputStream& GetOutput() override {
        return Output;
    }

    IInputStream& GetInput() override {
        return Input;
    }

private:
    void InitTaskMeta() {
        Yql::DqsProto::TTaskMeta taskMeta;
        Task.GetMeta().UnpackTo(&taskMeta);

        for (const auto& x : taskMeta.GetSecureParams()) {
            SecureParams[x.first] = x.second;
        }

        for (const auto& x : taskMeta.GetTaskParams()) {
            TaskParams[x.first] = x.second;
        }
    }

private:
    const TString TraceId;
    NDqProto::TDqTask Task;
    THashMap<TString, TString> SecureParams;
    THashMap<TString, TString> TaskParams;

    NKikimr::NMiniKQL::TScopedAlloc Alloc;
    NKikimr::NMiniKQL::TTypeEnvironment TypeEnv;
    NKikimr::NMiniKQL::TMemoryUsageInfo MemInfo;
    NKikimr::NMiniKQL::THolderFactory HolderFactory;

    std::atomic<bool> Running;
    int Code = -1;
    TString Stderr;
    THolder<TChildProcess> Command;
    THolder<TThread> StderrReader;
    IOutputStream& Output;
    IInputStream& Input;

    i32 ProtocolVersion = -1;
    const ui64 TaskId;
    const ui64 StageId;
};

class TDqTaskRunner: public NDq::IDqTaskRunner {
public:
    TDqTaskRunner(
        const NDqProto::TDqTask& task,
        THolder<TChildProcess>&& command,
        ui64 stageId,
        const TString& traceId)
        : Delegate(new TTaskRunner(task, std::move(command), stageId, traceId))
        , Task(task)
    { }

    ui64 GetTaskId() const override {
        return Task.GetId();
    }

    void Prepare(const NDqProto::TDqTask& task, const TDqTaskRunnerMemoryLimits& memoryLimits,
        const IDqTaskRunnerExecutionContext& execCtx, const TDqTaskRunnerParameterProvider&) override
    {
        Y_UNUSED(memoryLimits);
        Y_UNUSED(execCtx);
        Y_VERIFY(Task.GetId() == task.GetId());
        try {
            auto result = Delegate->Prepare();
            Y_UNUSED(result);
        } catch (...) {
            Delegate->RaiseException();
        }
    }

    ERunStatus Run() override
    {
        try {
            auto response = Delegate->Run();
            return static_cast<NDq::ERunStatus>(response.GetResult());
        } catch (...) {
            Delegate->RaiseException();
        }
    }

    bool HasEffects() const override
    {
        return false;
    }

    IDqInputChannel::TPtr GetInputChannel(ui64 channelId) override
    {
        auto& channel = InputChannels[channelId];
        if (!channel) {
            channel = new TDqInputChannel(
                Delegate->GetInputChannel(channelId),
                Task.GetId(),
                channelId,
                Delegate.Get());
            Stats.InputChannels[channelId] = channel->GetStats();
        }
        return channel;
    }

    IDqSource::TPtr GetSource(ui64 inputIndex) override {
        auto& source = Sources[inputIndex];
        if (!source) {
            source = new TDqSource(
                Task.GetId(),
                inputIndex,
                Delegate.Get());
            Stats.Sources[inputIndex] = source->GetStats();
        }
        return source;
    }

    IDqOutputChannel::TPtr GetOutputChannel(ui64 channelId) override
    {
        auto& channel = OutputChannels[channelId];
        if (!channel) {
            channel = new TDqOutputChannel(
                Delegate->GetOutputChannel(channelId),
                Task.GetId(),
                channelId,
                Delegate.Get());
            Stats.OutputChannels[channelId] = channel->GetStats();
        }
        return channel;
    }

    IDqSink::TPtr GetSink(ui64 outputIndex) override {
        auto& sink = Sinks[outputIndex];
        if (!sink) {
            sink = new TDqSink(
                Task.GetId(),
                outputIndex,
                Delegate.Get());
            // Stats.Sinks[outputIndex] = sink->GetStats();
        }
        return sink;
    }

    const NKikimr::NMiniKQL::TTypeEnvironment& GetTypeEnv() const override {
        return Delegate->GetTypeEnv();
    }

    const NKikimr::NMiniKQL::THolderFactory& GetHolderFactory() const override {
        return Delegate->GetHolderFactory();
    }

    const THashMap<TString, TString>& GetSecureParams() const override {
        return Delegate->GetSecureParams();
    }

    const THashMap<TString, TString>& GetTaskParams() const override {
        return Delegate->GetTaskParams();
    }

    TGuard<NKikimr::NMiniKQL::TScopedAlloc> BindAllocator(TMaybe<ui64> memoryLimit) override {
        return Delegate->BindAllocator(memoryLimit);
    }

    bool IsAllocatorAttached() override {
        return Delegate->IsAllocatorAttached();
    }

    void UpdateStats() override {
    }

    const TDqTaskRunnerStats* GetStats() const override
    {
        try {
            NDqProto::TCommandHeader header;
            header.SetVersion(3);
            header.SetCommand(NDqProto::TCommandHeader::GET_STATS);
            header.SetTaskId(Task.GetId());
            header.Save(&Delegate->GetOutput());

            NDqProto::TGetStatsResponse response;
            response.Load(&Delegate->GetInput());

            FromProto(&Stats, response.GetStats());

            return &Stats;
        } catch (...) {
            Delegate->RaiseException();
        }
    }

    TString Save() const override {
        ythrow yexception() << "unimplemented";
    }

    void Load(TStringBuf in) override {
        Y_UNUSED(in);
        ythrow yexception() << "unimplemented";
    }

private:
    TIntrusivePtr<TTaskRunner> Delegate;
    NDqProto::TDqTask Task;
    mutable TDqTaskRunnerStats Stats;

    THashMap<ui64, IDqInputChannel::TPtr> InputChannels;
    THashMap<ui64, IDqSource::TPtr> Sources;
    THashMap<ui64, IDqOutputChannel::TPtr> OutputChannels;
    THashMap<ui64, IDqSink::TPtr> Sinks;
};

/*______________________________________________________________________________________________*/


class TPipeFactory: public IProxyFactory {
    struct TJob: public TTaskScheduler::ITask {
        TJob(NThreading::TPromise<void> p)
            : Promise(std::move(p))
        { }

        TInstant Process() override {
            Promise.SetValue();
            return TInstant::Max();
        }

        NThreading::TPromise<void> Promise;
    };

    struct TStopJob: public TTaskScheduler::ITask {
        TList<THolder<TChildProcess>> StopList;

        TStopJob(TList<THolder<TChildProcess>>&& stopList)
            : StopList(std::move(stopList))
        { }

        TInstant Process() override {
            for (const auto& job : StopList) {
                job->Kill();
                job->Wait(TDuration::Seconds(1));
            }

            return TInstant::Max();
        }
    };

public:
    TPipeFactory(const TPipeFactoryOptions& options)
        : ExePath(options.ExecPath)
        , EnablePorto(options.EnablePorto)
        , PortoSettings({
                EnablePorto,
                TDqSettings::TDefault::PortoMemoryLimit,
                options.PortoLayer,
                options.ContainerName
            })
        , FileCache(options.FileCache)
        , Args {"yql@child", "tasks_runner_proxy"}
        , Env(options.Env)
        , Revision(GetProgramCommitId())
        , TaskScheduler(1)
        , MaxProcesses(options.MaxProcesses)
        , PortoCtlPath(options.PortoCtlPath)
    {
        Start(ExePath, PortoSettings);
        TaskScheduler.Start();
    }

    ITaskRunner::TPtr GetOld(const NDqProto::TDqTask& tmp, const TString& traceId) override {
        Yql::DqsProto::TTaskMeta taskMeta;
        tmp.GetMeta().UnpackTo(&taskMeta);
        ui64 stageId = taskMeta.GetStageId();
        auto result = GetExecutorForTask(taskMeta.GetFiles(), taskMeta.GetSettings());
        auto task = PrepareTask(tmp, result.Get());
        return new TTaskRunner(task, std::move(result), stageId, traceId);
    }

    TIntrusivePtr<NDq::IDqTaskRunner> Get(const NDqProto::TDqTask& tmp, const TString& traceId) override
    {
        Yql::DqsProto::TTaskMeta taskMeta;
        tmp.GetMeta().UnpackTo(&taskMeta);

        auto result = GetExecutorForTask(taskMeta.GetFiles(), taskMeta.GetSettings());
        auto task = PrepareTask(tmp, result.Get());
        return new TDqTaskRunner(task, std::move(result), taskMeta.GetStageId(), traceId);
    }

private:
    THolder<TChildProcess> StartOne(const TString& exePath, const TPortoSettings& portoSettings) {
        return CreateChildProcess(PortoCtlPath, FileCache->GetDir(), exePath, Args, Env, ContainerId++, portoSettings);
    }

    void Start(const TString& exePath, const TPortoSettings& portoSettings) {
        const auto key = GetKey(exePath, portoSettings);
        while (static_cast<int>(ProcessHolder.Size()) < MaxProcesses) {
            auto process = StartOne(exePath, portoSettings);
            ProcessHolder.Put(key, std::move(process));
        }
    }

    void ProcessJobs(const TString& exePath, const TPortoSettings& portoSettings) {
        NThreading::TPromise<void> promise = NThreading::NewPromise();

        promise.GetFuture().Apply([=](const NThreading::TFuture<void>&) mutable {
            Start(exePath, portoSettings);
        });

        Y_VERIFY(TaskScheduler.Add(MakeIntrusive<TJob>(promise), TInstant()));
    }

    void StopJobs(TList<THolder<TChildProcess>>&& stopList) {
        Y_VERIFY(TaskScheduler.Add(MakeIntrusive<TStopJob>(std::move(stopList)), TInstant()));
    }

    TString GetKey(const TString& exePath, const TPortoSettings& settings)
    {
        return exePath + "," + settings.ToString();
    }

    NDqProto::TDqTask PrepareTask(const NDqProto::TDqTask& tmp, TChildProcess* result) {
        // get files from fileCache
        NDqProto::TDqTask task = tmp;
        Yql::DqsProto::TTaskMeta taskMeta;

        task.GetMeta().UnpackTo(&taskMeta);

        auto* files = taskMeta.MutableFiles();
        THashSet<TString> initialized;

        for (auto& file : *files) {
            if (file.GetObjectType() != Yql::DqsProto::TFile::EEXE_FILE) {
                if (initialized.contains(file.GetObjectId())) {
                    file.SetObjectType(Yql::DqsProto::TFile::EEXE_FILE); // skip this in runner
                    continue;
                }
                initialized.insert(file.GetObjectId());

                auto maybeFile = FileCache->FindFile(file.GetObjectId());
                if (!maybeFile) {
                    throw std::runtime_error("Cannot find object `" + file.GetObjectId() + "' in cache");
                }
                auto name = file.GetName();

                switch (file.GetObjectType()) {
                    case Yql::DqsProto::TFile::EEXE_FILE:
                        break;
                    case Yql::DqsProto::TFile::EUDF_FILE:
                    case Yql::DqsProto::TFile::EUSER_FILE:
                        file.SetLocalPath(InitializeLocalFile(result->ExternalWorkDir(), *maybeFile, name));
                        break;
                    default:
                        Y_VERIFY(false);
                }
            }
        }

        auto& taskParams = *taskMeta.MutableTaskParams();
        taskParams[WorkingDirectoryParamName] = result->InternalWorkDir();
        taskParams[WorkingDirectoryDontInitParamName] = "true";
        task.MutableMeta()->PackFrom(taskMeta);

        return task;
    }

    TFsPath MakeLocalPath(TString fileName) {
        TString localPath = fileName;
        if (localPath.StartsWith(TStringBuf("/home/"))) {
            localPath = localPath.substr(TStringBuf("/home").length()); // Keep leading slash
        }
        if (!localPath.StartsWith('/')) {
            localPath.prepend('/');
        }
        localPath.prepend('.');
        return localPath;
    }

    TString InitializeLocalFile(const TString& base, const TString& path, const TFsPath& name)
    {
        auto localPath = MakeLocalPath(name.GetPath());
        NFs::MakeDirectoryRecursive(base + "/" + localPath.Parent().GetPath(), NFs::FP_NONSECRET_FILE, false);
        YQL_LOG(DEBUG) << "HardLink '" << path << "-> '" << (base + "/" + localPath.GetPath()) << "'";
        YQL_ENSURE(NFs::HardLink(path, base + "/" + localPath.GetPath()));
        return localPath.GetPath();
    }

    template<typename T, typename S>
    THolder<TChildProcess> GetExecutorForTask(const T& files, const S& settings) {
        TString executorId;
        TPortoSettings portoSettings = PortoSettings;

        for (const auto& file : files) {
            if (file.GetObjectType() == Yql::DqsProto::TFile::EEXE_FILE) {
                executorId = file.GetObjectId();
                break;
            }
        }

        TDqConfiguration::TPtr conf = new TDqConfiguration;
        try {
            conf->Dispatch(settings);
        } catch (...) { /* ignore unknown settings */ }
        portoSettings.Enable = EnablePorto && conf->EnablePorto.Get().GetOrElse(TDqSettings::TDefault::EnablePorto);
        portoSettings.MemoryLimit = conf->_PortoMemoryLimit.Get().GetOrElse(TDqSettings::TDefault::PortoMemoryLimit);
        if (portoSettings.Enable) {
            YQL_LOG(DEBUG) << "Porto enabled";
        }

        TString exePath;
        if (executorId.empty() || executorId == Revision) {
            exePath = ExePath;
        } else {
            auto maybeExeFile = FileCache->FindFile(executorId);
            if (!maybeExeFile) {
                throw std::runtime_error("Cannot find exe `" + executorId + "' in cache");
            }
            exePath = *maybeExeFile;
        }

        auto key = GetKey(exePath, portoSettings);
        TList<THolder<TChildProcess>> stopList;
        THolder<TChildProcess> result = ProcessHolder.Acquire(key, &stopList);
        if (!result) {
            result = StartOne(exePath, portoSettings);
        }
        ProcessJobs(exePath, portoSettings);
        StopJobs(std::move(stopList));
        return result;
    }

    static THolder<TChildProcess> CreateChildProcess(
        const TString& portoCtlPath,
        const TString& cacheDir,
        const TString& exePath,
        const TVector<TString>& args,
        const THashMap<TString, TString>& env,
        i64 containerId,
        const TPortoSettings& portoSettings)
    {
        THolder<TChildProcess> command;
        if (portoSettings.Enable) {
            command = MakeHolder<TPortoProcess>(portoCtlPath, exePath, args, env, cacheDir + "/Slot-" + ToString(containerId), portoSettings);
        } else {
            command = MakeHolder<TChildProcess>(exePath, args, env, cacheDir + "/Slot-" + ToString(containerId));
        }
        YQL_LOG(DEBUG) << "Executing " << exePath;
        for (const auto& arg: args) {
            YQL_LOG(DEBUG) << "Arg: " << arg;
        }
        command->Run();
        return command;
    }

    std::atomic<i64> ContainerId = 1;

    const TString ExePath;
    const bool EnablePorto;
    const TPortoSettings PortoSettings;
    const IFileCache::TPtr FileCache;
    const TString UdfsDir;
    const TVector<TString> Args;
    const THashMap<TString, TString> Env;

    const TString Revision;

    TProcessHolder ProcessHolder;
    TTaskScheduler TaskScheduler;
    const int MaxProcesses;
    const TString PortoCtlPath;
};

IProxyFactory::TPtr CreatePipeFactory(
    const TPipeFactoryOptions& options)
{
    return new TPipeFactory(options);
}

} // namespace NYql::NTaskRunnerProxy
