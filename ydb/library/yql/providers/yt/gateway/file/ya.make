LIBRARY()

SRCS(
    yql_yt_file_comp_nodes.cpp
    yql_yt_file_mkql_compiler.cpp
    yql_yt_file_services.cpp
    yql_yt_file.cpp
)

PEERDIR(
    library/cpp/yson
    library/cpp/yson/node
    yt/cpp/mapreduce/common
    yt/cpp/mapreduce/interface
    ydb/library/yql/core/file_storage
    ydb/library/yql/core/file_storage/proto
    ydb/library/yql/core/file_storage/http_download
    ydb/library/yql/minikql/comp_nodes/llvm
    ydb/library/yql/public/udf
    ydb/library/yql/utils
    ydb/library/yql/utils/threading
    ydb/library/yql/core
    ydb/library/yql/core/expr_nodes
    ydb/library/yql/core/type_ann
    ydb/library/yql/providers/common/codec
    ydb/library/yql/providers/common/comp_nodes
    ydb/library/yql/providers/common/gateway
    ydb/library/yql/providers/common/mkql
    ydb/library/yql/providers/common/provider
    ydb/library/yql/providers/common/schema/expr
    ydb/library/yql/providers/common/schema/mkql
    ydb/library/yql/providers/result/expr_nodes
    ydb/library/yql/providers/yt/common
    ydb/library/yql/providers/yt/comp_nodes
    ydb/library/yql/providers/yt/expr_nodes
    ydb/library/yql/providers/yt/gateway/lib
    ydb/library/yql/providers/yt/lib/infer_schema
    ydb/library/yql/providers/yt/lib/lambda_builder
    ydb/library/yql/providers/yt/lib/mkql_helpers
    ydb/library/yql/providers/yt/lib/res_pull
    ydb/library/yql/providers/yt/lib/schema
    ydb/library/yql/providers/yt/lib/yson_helpers
    ydb/library/yql/providers/yt/provider
    ydb/library/yql/parser/pg_wrapper
)

YQL_LAST_ABI_VERSION()

END()
