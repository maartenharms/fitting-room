#include "VmCall.h"

namespace OS::VmCall {

    namespace {

        // Is a_function a static on a_type, by the same rule the engine uses?
        //
        // ⚠ THE LINK STATE IS PART OF THE QUESTION, NOT A PRECONDITION. A type
        // that is kNotLinked or kCurrentlyLinking keeps an UnlinkedNativeFunction
        // list where the linked layout keeps its tables, so GetGlobalFuncIter on
        // one of those reads a list pointer as a function array. The engine
        // refuses a static outright unless the state is kLinkedValid; anything
        // else is a "no" here for the same reason.
        [[nodiscard]] bool HasStatic(const RE::BSScript::ObjectTypeInfo* a_type,
                                     const RE::BSFixedString& a_function) {
            if (!a_type ||
                a_type->linkedValid !=
                    RE::BSScript::ObjectTypeInfo::LinkValidState::kLinkedValid) {
                return false;
            }
            const auto* const iter  = a_type->GetGlobalFuncIter();
            const auto        count = a_type->GetNumGlobalFuncs();
            if (!iter) {
                return false;
            }
            for (std::uint32_t i = 0; i < count; ++i) {
                const auto& fn = iter[i].func;
                if (fn && fn->GetName() == a_function) {
                    return true;
                }
            }
            return false;
        }

    }  // namespace

    bool Static(
        RE::BSScript::IVirtualMachine*    a_vm,
        const RE::BSFixedString&          a_class,
        const RE::BSFixedString&          a_function,
        RE::BSScript::IFunctionArguments* a_args,
        RE::BSTSmartPointer<RE::BSScript::IStackCallbackFunctor>& a_callback) {
        if (!a_vm) {
            delete a_args;
            return false;
        }

        // The class. This is the half the engine already answers safely, and
        // asking it ourselves costs one lookup and keeps both answers in one
        // place, so a caller reads a single false rather than two kinds of it.
        RE::BSTSmartPointer<RE::BSScript::ObjectTypeInfo> type;
        if (!a_vm->GetScriptObjectType(a_class, type) || !type) {
            spdlog::debug("VmCall: no script class '{}'; {} not called.",
                          a_class.c_str(), a_function.c_str());
            delete a_args;
            return false;
        }

        // The function. This is the half that used to be a crash.
        if (!HasStatic(type.get(), a_function)) {
            spdlog::warn(
                "VmCall: '{}' is installed but carries no static {}, so the "
                "call is dropped. The engine would have dereferenced a null "
                "here rather than refusing, so this is a version of that mod "
                "older than the function we want.",
                a_class.c_str(), a_function.c_str());
            delete a_args;
            return false;
        }

        return a_vm->DispatchStaticCall(a_class, a_function, a_args,
                                        a_callback);
    }

}  // namespace OS::VmCall
