#pragma once

#include <util/generic/set.h>

#include "ydb_command.h"

namespace NYdb::NConsoleClient {

class TClickBenchCommandInit : public NYdb::NConsoleClient::TYdbCommand {
public:
    TClickBenchCommandInit();
    void Config(TConfig& config);
    int Run(TConfig& config);

private:
    TString Path;
    TString Table;
};

class TClickBenchCommandRun : public NYdb::NConsoleClient::TYdbCommand {
protected:
    TSet<ui32> QueriesToRun;
    TSet<ui32> QueriesToSkip;
    bool DisableLlvm = false;
    bool EnablePushdown = false;
    TString ExternalQueries;
    TString ExternalQueriesFile;

public:
    TClickBenchCommandRun();
    void Config(TConfig& config);
    int Run(TConfig& config);
    TString PatchQuery(const TStringBuf& original) const;
    bool NeedRun(const ui32 queryIdx) const;
    bool RunBench(TConfig& config);

    TString GetQueries(const TString& fullTablePath) const;

    TString OutFilePath;
    ui32 IterationsCount;
    TString JsonReportFileName;
    TString Path;
    TString Table;
};

class TCommandClickBench : public NYdb::NConsoleClient::TClientCommandTree {
public:
    TCommandClickBench();
};

}  // namespace NYdb::NConsoleClient
