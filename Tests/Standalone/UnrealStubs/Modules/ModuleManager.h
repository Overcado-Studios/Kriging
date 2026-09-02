#pragma once

class IModuleInterface
{
public:
    virtual ~IModuleInterface() = default;
};

#define IMPLEMENT_MODULE(ModuleClass, ModuleName) \
    static_assert(true, "Unreal module source syntax stub");
