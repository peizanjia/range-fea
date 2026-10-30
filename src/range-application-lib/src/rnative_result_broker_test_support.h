#ifndef RNATIVE_RESULT_BROKER_TEST_SUPPORT_H
#define RNATIVE_RESULT_BROKER_TEST_SUPPORT_H

#if defined(RANGE_APPLICATION_TEST_HOOKS)
#include <functional>
#include <QString>

namespace RNativeResultBrokerTestSupport
{
enum class FrameParsePhase
{
    BeforeParse,
    AfterParse
};

using FrameParseHook = std::function<void(const QString &, FrameParsePhase)>;

//! Install a one-test-only hook around frame parsing. Built only with RANGE_BUILD_TESTS.
void setFrameParseHookForTesting(FrameParseHook hook);
}
#endif

#endif
