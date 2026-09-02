#pragma once

#include <string>

#define TEXT(Value) Value
#define UTF8_TO_TCHAR(Value) Value

class FString
{
public:
    FString() = default;
    explicit FString(const char* In) : Value(In ? In : "") {}

    static FString Printf(const char*, const char* Value)
    {
        return FString(Value);
    }

private:
    std::string Value;
};

enum class EAutomationTestFlags
{
    EditorContext = 1,
    EngineFilter = 2
};

inline EAutomationTestFlags operator|(EAutomationTestFlags Left,
                                      EAutomationTestFlags Right)
{
    return static_cast<EAutomationTestFlags>(
        static_cast<int>(Left) | static_cast<int>(Right));
}

#define IMPLEMENT_SIMPLE_AUTOMATION_TEST(ClassName, PrettyName, Flags) \
    class ClassName \
    { \
    public: \
        bool RunTest(const FString& Parameters); \
        void TestTrue(const char*, bool) {} \
        void AddError(const FString&) {} \
    };
