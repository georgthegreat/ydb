LIBRARY()

INCLUDE(${ARCADIA_ROOT}/yt/ya_cpp.make.inc)

SRCS(
    config.cpp
    counter.cpp
    hedging.cpp
    hedging_executor.cpp
    logger.cpp
    penalty_provider.cpp
)

PEERDIR(
    yt/yt/core
    yt/yt/client
    yt/yt/client/cache
    yt/yt/library/profiling
    yt/yt_proto/yt/client/hedging

    library/cpp/iterator
)

END()

RECURSE_FOR_TESTS(unittests)
