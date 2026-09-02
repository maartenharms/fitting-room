#pragma once
#include "PCH.h"

namespace OS::VmCall {

    // A static Papyrus call that answers "no" where the engine's own would
    // crash.
    //
    // ⚠⚠ IVirtualMachine::DispatchStaticCall IS ONLY HALF SAFE, AND WE SHIPPED
    // INTO THE OTHER HALF. It handles a MISSING CLASS: its first act is
    // GetScriptObjectType, and a class nothing in the load order provides makes
    // it return false with the whole body skipped. It does NOT handle a class
    // that exists WITHOUT THE FUNCTION. There it calls ObjectTypeInfo::
    // GetFunction (AE 104311), which returns false and leaves the out pointer
    // exactly as the caller left it, then ignores that answer and dereferences
    // the null anyway:
    //
    //     MOV RCX, qword ptr [RSP+0x40]   ; GetFunction's untouched out-param
    //     MOV RAX, qword ptr [RCX]        ; rcx = 0
    //     CALL qword ptr [RAX+0x50]
    //
    // AE 104802 (VirtualMachine::vf38) +0xAE. There is no version of this the
    // caller can survive, so the only fix is not to make the call.
    //
    // ⚠ THIS WAS A SHIPPED CTD, NOT A THEORY. A player with no CBPC.dll but a
    // stale cbpcPluginScript.pex somewhere in the load order, which body and
    // physics packs bundle all the time, crashed every single time they put an
    // outfit on biped slot 32, and on nothing else, because slot 32 is the only
    // slot that reaches CbpcArmorClass's dispatch. Their class resolved and
    // their copy had no ApplyBounceInterpolation (reported 2026-08-29, crash
    // log read, cause confirmed against the disassembly rather than guessed).
    //
    // ⚠ THE PROBE MIRRORS THE ENGINE'S OWN LOOKUP AND MUST NOT BE WIDENED.
    // GetFunction resolves a STATIC through one path only: linkedValid must be
    // kLinkedValid, and then it searches THIS type's global function table and
    // no parent's (AE 104355 walks a fixed offset into the type's own data
    // block, sized by its own staticFunctionCount). A probe that searched
    // parents, or that accepted a type mid-link, would say yes where the engine
    // says no, and saying yes wrongly is the crash we are here to stop. Saying
    // no wrongly costs a feature that was never going to run.
    //
    // Returns what DispatchStaticCall returns when the call is actually made,
    // and false when the class or the function is not there. a_args is consumed
    // either way: it is deleted on the refusal paths rather than leaked.
    bool Static(
        RE::BSScript::IVirtualMachine*    a_vm,
        const RE::BSFixedString&          a_class,
        const RE::BSFixedString&          a_function,
        RE::BSScript::IFunctionArguments* a_args,
        RE::BSTSmartPointer<RE::BSScript::IStackCallbackFunctor>& a_callback);

}  // namespace OS::VmCall
