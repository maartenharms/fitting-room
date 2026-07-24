#pragma once

#include <limits>
#include <string_view>

/**
    This header is a C++ API for interoperating with OBody via a SKSE plugin.

    The expected usage of this header is for you to copy it wholesale into your project and use it as-is.
    There is no need to involve any other portion of OBody's source code for use of this API.

    Skyrim game types, such as `Actor` and `TESForm` are used by this header, but are left undefined
    so that you can define them yourself before including this header. Such as by including the
    headers of the likes of CommonLibSSE or SKSE64, for example.

    The overall usage of this API is relatively simple:
        After SKSE has sent a `kPostPostLoad` message to your plugin, you can send a `RequestPluginInterface`
        message along with an `IOBodyReadinessEventListener` instance to OBody, and if OBody is installed,
        and your request is valid, OBody will write a pointer to an `IPluginInterface` instance
        through your supplied pointer.
        That object is your primary gateway to interoperating with OBody.

        But do note that an `IPluginInterface` instance can be used only when it is safe to do so--
        you can be notified of when it is safe to do so via the `IOBodyReadinessEventListener` instance
        that you supply in a `RequestPluginInterface` message.

        For example, this is how you would request a plugin interface when using CommonLibSSE:
        ```
            if (message->type == SKSE::MessagingInterface::kPostPostLoad) {
                OBody::API::SKSEMessages::RequestPluginInterface req{};
                req.version = OBody::API::PluginAPIVersion::Latest;
                req.pluginInterface = &yourGlobalState.obodyAPI;
                req.readinessEventListener = &yourGlobalState.obodyReadinessListener;

                assert(yourGlobalState.obodyAPI == nullptr);

                SKSE::GetMessagingInterface()->Dispatch(decltype(req)::type, &req, sizeof(decltype(req)), "OBody");

                if (yourGlobalState.obodyAPI != nullptr) {
                    // OBody is installed!
                }
            }

            // ...

            class OBodyReadinessListener : public OBody::API::IOBodyReadinessEventListener {
            public:
                bool initialised = false;

                virtual void OBodyIsReady() override {
                    yourGlobalState.obodyIsSafeToUse = true;

                    if (!this->initialised) {
                        yourGlobalState.obodyAPI->RegisterEventListener(yourGlobalState.actorChangeListener);
                        this->initialised = true;
                    }
                }

                virtual void OBodyIsNoLongerReady() override {
                    yourGlobalState.obodyIsSafeToUse = false;
                }
            };
        ```

        See the end of this file for the licence that this header file is made available under.
*/

/* A note for implementers:
   Be mindful of ABI-compatibility when making changes to this header, if you do any of the following
   without introducing a new `PluginAPIVersion` and corresponding versioned virtual classes, you risk
   breaking mods that use this API:
     * Changing the order of virtual methods or members in an aggregate type.
     * Removing virtual methods or members from an aggregate type.
     * Changing the values of an enum type or of constant values.
     * Changing how parameters are passed to a function, or how its result is returned;
         see: https://learn.microsoft.com/cpp/build/x64-calling-convention
     * Increasing the required alignment of an aggregate type.

   Stick to appending virtual methods and members to the end of aggregate types and all will be grand.
*/

namespace OBody::API {
    /** This represents a version of the OBody plugin-API and is unrelated to the version of OBody proper.
        These version numbers signify how this API is to be used.
        New versions will be introduced when breaking changes are made to the API, so that it's feasible to
        update the API without breaking SKSE plugins that were compiled for older versions.
    */
    enum class PluginAPIVersion { Invalid = 0, v1 = 1, Latest = v1 };

    class IActorChangeEventListener;
    struct PresetCounts;
    enum PresetCategory : uint64_t;
    struct PresetAssignmentInformation;
    struct AssignPresetPayload;

    /** See the documentation for `IPluginInterface`, this is its base class purely to make it
        easier to maintain ABI-compatibility.

        As suggested by the name of this class, it's layout will remain
        compatible with all versions of the OBody plugin API.
    */
    class IPluginInterfaceVersionIndependent {
    public:
        /** This is a string that identifies the mod which requested this `IPluginInterface`.
            By default, this is the name of the mod's SKSE plugin.

            Do not change this string directly, if you must change it: use the `SetOwner` method.
            This is exposed as a field solely to reduce the overhead of reading it.

            This string must remain valid for the duration of the `IPluginInterface`'s lifetime. */
        const char* owner;

        /** This is a pointer-sized field that you can use for whatever you want.
            OBody does not care about the value of the field, it does not use it.
            By default, this is null. */
        void* context = nullptr;

        /** This returns the version of the OBody plugin-API that this `IPluginInterface` implements. */
        virtual PluginAPIVersion PluginAPIVersion() = 0;

        /** This is used to change the value of the `owner`field. It returns the value of the `owner` parameter. */
        virtual const char* SetOwner(const char* owner) = 0;
    };

    /** This is the primary interface of the plugin-API.
        This is what you get in return from sending a `RequestPluginInterface` message to `OBody`.

        You can acquire a plugin interface after SKSE has sent a `kPostPostLoad` message to your plugin,
        but note that it will not be safely usable until OBody sends your `IOBodyReadinessEventListener` instance
        an `OBodyIsReady` event.

        Unless otherwise stated, all the methods provided by this type are thread-safe.
    */
    class IPluginInterface : public IPluginInterfaceVersionIndependent {
    public:
        /** This is used to check whether OBody considers an actor to be naked or not.
            If you're calling this in the context of a `TESEquipEvent`
            event sink, see the 3-argument overload of this method. */
        virtual bool ActorIsNaked(Actor* actor) = 0;

        /** This is used to check whether OBody considers an actor to be naked or not.
            If you're calling this in the context of a `TESEquipEvent`
            event sink, note that the game will not yet have actually [un]equipped the form from
            the actor, thus you'll need to pass to OBody whether the event is an unequip or an equip event,
            and the armor that's being [un]equipped so that OBody can properly assess whether the
            actor is naked or not. */
        virtual bool ActorIsNaked(Actor* actor, bool actorIsEquippingArmor, const TESForm* armor) = 0;

        /** This is used to check whether ORefit is currently applied to an actor or not. */
        virtual bool ActorHasORefitApplied(Actor* actor) = 0;

        /** This is used to check whether OBody has processed an actor or not.
            Wherein an actor is considered processed if they have OBody morphs
            for the current distribution applied to them.
            A blacklisted actor may be considered as processed. */
        virtual bool ActorIsProcessed(Actor* actor) = 0;

        /** This is used to check whether OBody has blacklisted an actor or not.
            A blacklisted actor is an actor that OBody is not automatically applying presets to.
            A user may manually apply a preset to a blacklisted actor. */
        virtual bool ActorIsBlacklisted(Actor* actor) = 0;

        /** This is used to check whether ORefit is globally enabled for OBody or not. */
        virtual bool IsORefitEnabled() = 0;

        /** `RegisterEventListener` and `DeregisterEventListener` are used
             to subscribe and unsubscribe to and from events from OBody.

             Registering and deregistering event-listeners acquires an exclusive lock internally,
             so if you want to disable and re-enable an event-listener frequently, you should do
             so internally in that event-listener.

             Whilst these methods are thread-safe, you MUST not call these methods for
             an `IXYZEventListener` instance from within the context
             of an `IXYZEventListener` method called by OBody,
             because doing so will invalidate the iterator that OBody is acting upon.

             Rely on the order in which OBody sends events to different listeners for the same event
             at your own peril.
             The order in which OBody send different events in is deterministic.
        */

        /** This will make OBody start sending events to `eventListener`,
            returning whether the registration was successful or not.
            If this is called multiple times with the same listener, that listener will receive duplicated events.

            The `eventListener` reference passed to this method MUST remain valid until it is passed
            to `DeregisterEventListener` (or until the game's process terminates). */
        virtual bool RegisterEventListener(IActorChangeEventListener& eventListener) = 0;

        /** This will make OBody stop sending events to `eventListener`,
            returning whether any listeners were deregistered or not. */
        virtual bool DeregisterEventListener(IActorChangeEventListener& eventListener) = 0;

        /** This is used to check whether OBody is sending events to `eventListener` or not. */
        virtual bool HasRegisteredEventListener(IActorChangeEventListener& eventListener) = 0;

        /** This is used to get the number of presets that OBody recognises.
            Every field of `payload` will be set by this function, you needn't initialise it. */
        virtual void GetPresetCounts(PresetCounts& payload) = 0;

        /** This is used to get a selection of the names of the presets recognised by OBody,
            for a specific category of presets.

            To use this function, supply a pointer to a contiguous span of `std::string_view`s
            via the `buffer` parameter, and supply the length of that buffer via the `bufferLength` parameter.
            This function will place as many preset names into your buffer as it can,
            returning the number of preset names that it placed.

            The `string_view`s that this function copies into your supplied buffer are guaranteed to
            remain valid until OBody sends an `OBodyIsNoLongerReady` event to your `IOBodyReadinessEventListener`.
            Additionally, those `string_view`s are guaranteed to be non-null,
            and will point to null-terminated strings.

            By default, this copies all the presets available, the `offset` and `limit` parameters
            can be used to return only a subset of the preset names. */
        virtual size_t GetPresetNames(PresetCategory category, std::string_view* buffer, size_t bufferLength,
                                      size_t offset = 0, size_t limit = (std::numeric_limits<size_t>::max)()) = 0;

        /** This is used to ensure that OBody processes an actor for the current distribution key.
            That is to say, this operation does nothing if the actor has already been processed
            by OBody for the current distribution key, otherwise it will force OBody to process
            the actor for the current distribution key, in accordance with OBody's configuration. */
        virtual void EnsureActorIsProcessed(Actor* actor) = 0;

        /** This is used to reapply any OBody morphs that are or were applied to an actor,
            such that the actor's morph will be as they should, according to the preset assigned to
            them; if no preset is assigned to them, a preset will be assigned to them in the usual
            fashion. `RemoveOBodyMorphsFromActor` can be used to reverse this operation. */
        virtual void ApplyOBodyMorphsToActor(Actor* actor) = 0;

        /** This is used to remove any OBody morphs that are applied to an actor, such that the
            actor's morph will be as though OBody had never morphed the actor at all.
            `ApplyOBodyMorphsToActor` can be used to reverse this operation.
            Any per-actor configuration, such as the applied preset, will be retained for the actor. */
        virtual void RemoveOBodyMorphsFromActor(Actor* actor) = 0;

        /** This is used to forcefully change whether ORefit is applied or not to an actor,
            regardless of the actor's equipped armour, and without respect to the global setting
            for ORefit. */
        virtual void ForcefullyChangeORefitForActor(Actor* actor, bool orefitShouldBeApplied) = 0;

        /** This is used to get information about the preset currently assigned to an actor.
            You MUST initialise the `flags` field of `payload`, every other field may be left uninitialised. */
        virtual void GetPresetAssignedToActor(Actor* actor, PresetAssignmentInformation& payload) = 0;

        /** This is used to assign a preset to an actor.

            If the supplied preset name is an empty or null string this method will
            unassign any preset assigned to the actor.

            This returns whether a preset with the supplied name was found or not.
            If the supplied preset name was empty or null this will return `true`. */
        virtual bool AssignPresetToActor(Actor* actor, AssignPresetPayload& payload) = 0;
    };

    /** This is an interface for receiving events from OBody regarding
        whether OBody is ready for other mods to interact with it via the plugin-API or not.

        This event-listener MUST be used to be notified of when it is and isn't safe to use `IPluginInterface`s.

        At various stages of the game's life-cycle, OBody may need to rearrange its state,
        and during those periods usage of OBody's plugin-API via an `IPluginInterface` will be unsafe,
        causing bugs at best and memory corruption at worst (if multi-threading is involved).
        The most notable period wherein this is so is when a game is saved or loaded.

        If you want to safely interact with OBody's plugin-API in response to the game saving or loading,
        you should do so by reacting to these events.

        Note that when Obody calls the methods of an instance of this class, that method and the functions it calls
        MUST not send a `RequestPluginInterface` SKSE message to OBody,
        because doing so will invalidate the iterator that OBody is acting upon.
    */
    class IOBodyReadinessEventListener {
    public:
        /** The OBodyIsReady event is sent just after OBody has become ready for the plugin-API
            to be used and has sent an `OBodyIsBecomingReady` event to every `IOBodyReadinessEventListener`,
            or when OBody reponds to a `RequestPluginInterface` SKSE message when it is already ready.

            It is safe to use `IPluginInterface` instances from the moment this method is called.
        */
        virtual void OBodyIsReady() = 0;

        /** The OBodyIsNoLongerReady event is sent when OBody stops being ready for the plugin-API to be used.

            It is not safe to use `IPluginInterface` instances from the moment this method is called.
        */
        virtual void OBodyIsNoLongerReady() = 0;

        /** The OBodyIsBecomingReady event is sent just before OBody transitions from being unready
            to being ready,
            or when OBody reponds to a `RequestPluginInterface` SKSE message when it is already ready.

            It is safe to use `IPluginInterface` instances after every `IOBodyReadinessEventListener`
            has handled this event, which is signaled via the `OBodyIsReady` event.

            The purpose of this event is to give you a chance to set up any state that you may
            need to set up in order to handle events originating from other `IPluginInterface`s
            _before_ you have received the `OBodyIsReady` event.

            For an example of why that may be needed, consider this scenario:
                There are two mods using the OBody plugin-API: Mod-A, and Mod-B.
                The game was saved by the player, and so OBody became unready, and Mod-B tore down
                some of its state that it requires for its `IActorChangeEventListener` instance.
                OBody then becomes ready again, and Mod-B sets up its state in response to the
                `OBodyIsBecomingReady` event.
                Then, when Mod-A receives its `OBodyIsReady` event, it uses its `IPluginInterface` to change
                an actor, which causes Mod-B's `IActorChangeEventListener` to receive an event
                BEFORE Mod-B has had a chance to receive the `OBodyIsReady` event.
                If Mod-B hadn't had a chance to set up its state via the `OBodyIsBecomingReady` event
                a bug would have occurred.
        */
        virtual void OBodyIsBecomingReady() {};

        /** The OBodyIsBecomingUnready event is sent just before OBody transitions from being ready
            to being unready.

            It is safe to use `IPluginInterface` instances when this method is called,
            and it remains safe to do so until the `OBodyIsNoLongerReady` event is sent.

            This event is effectively OBody yelling, "Last orders, please!".
        */
        virtual void OBodyIsBecomingUnready() {};
    };

    struct PresetCounts {
        /** The number of non-blacklisted presets applicable to female actors. */
        uint32_t female;
        /** The number of blacklisted presets applicable to female actors. */
        uint32_t femaleBlacklisted;
        /** The number of non-blacklisted presets applicable to male actors. */
        uint32_t male;
        /** The number of blacklisted presets applicable to male actors. */
        uint32_t maleBlacklisted;
    };

    enum PresetCategory : uint64_t {
        PresetCategoryNone = 0,
        /** Specifies non-blacklisted presets applicable to female actors. */
        PresetCategoryFemale = 1 << 0,
        /** Specifies blacklisted presets applicable to female actors. */
        PresetCategoryFemaleBlacklisted = 1 << 1,
        /** Specifies non-blacklisted presets applicable to male actors. */
        PresetCategoryMale = 1 << 2,
        /** Specifies blacklisted presets applicable to male actors. */
        PresetCategoryMaleBlacklisted = 1 << 3
    };

    struct PresetAssignmentInformation {
        enum Flags : uint64_t {
            None = 0,
            /** This bit is set if the actor is female. */
            IsFemale = 1 << 0
        };

        /** A bitwise combination of flags regarding the preset assignment. */
        Flags flags = Flags::None;

        /** This is the name of a preset assigned to an actor;
            if no preset is assigned to the actor this will be an empty string.

            This is a `string_view`, but the string it points to is null-terminated,
            so it can be used as C-style string.
            The data that this `string_view` points to is guaranteed to be valid
            until OBody sends your `IOBodyReadinessEventListener` an `OBodyIsNoLongerReady` event. */
        std::string_view presetName;
    };

    struct AssignPresetPayload {
        enum Flags : uint64_t {
            None = 0,
            /** If this bit is set, OBody will immediately apply or remove the morphs
                for the assigned preset to the actor, instead of queuing the morphs to
                be applied later. */
            ForceImmediateApplicationOfMorphs = 1 << 0,
            /** If this bit is set, OBody will refrain from applying the morphs, and from
                queuing the morphs to be applied later, for a preset when assigning a preset to an
                actor. The morphs can be applied later by calling `ApplyOBodyMorphsToActor`. The flag
                takes precedence over the `ForceImmediateApplicationOfMorphs` flag.
                If the `presetName` field is a null or empty string this bit will instead prevent
                OBody from removing the morphs applied to the actor, if any are applied. */
            DoNotApplyMorphs = 1 << 1
        };

        /** A bitwise combination of flags regarding the assignment of the preset. */
        Flags flags = Flags::ForceImmediateApplicationOfMorphs;

        /** The name of a preset that is to be assigned to an actor. */
        std::string_view presetName;
    };

    /** This is an interface for receiving events from OBody regarding the state of actors.

        If you want to keep your plugin's state in sync with OBody's state for actors you should
        implement this class and pass an instance of it to `IPluginInterface::RegisterEventListener`.

        When registered with OBody, OBody will call the methods defined by an instance of this class
        to signal the occurrence of certain events.

        Note that when Obody calls the methods of an instance of this class, that method and the functions it calls
        MUST not call `IPluginInterface::RegisterEventListener` or `IPluginInterface::DeregisterEventListener`
        for an `IActorChangeEventListener` instance, because doing so will invalidate the iterator that OBody
        is acting upon.

        The virtual methods of this class all have default implementations, so you need only implement the events
        you care about.

        These events have a consistent interface; an actor is passed as the first parameter;
        then a 64-bit bit-packed structure; and then a reference to a payload with extra data.
        This maximises performance as all the arguments are passed via registers, and the payload structure
        can be expanded without breaking ABI compatibility.
        Every event returns a response, which OBody may or may not use.
        The payload is mutable as it may be used as an extended return-channel in the future, if needed.

        OBody aims to make it feasible to make changes to an actor in response to these events
        without causing catastrophic bugs.
        This is achieved primarily by these two means:
          A) Events are not sent recursively, on a per-actor basis. That is, if an `IActorChangeEventListener`,
             in the act of responding to an event for a given actor,
             does something that would typically cause events to be sent to `IActorChangeEventListener`s:
             those events are not sent.
          B) The state passed to event-listeners via the `flags` and `payload` parameters are not updated
             by OBody between the calls to each event-listener's method: those values are effectively
             frozen in time, to be as they were before any event-listeners made any changes.
             If an event-listener wants the most up-to-date true state, it must go out of its way to call the
             appropriate methods via its `IPluginInterface`.

        To elucidate why this is done, consider the following scenarios:
            We have Mod-A and Mod-B which have both registered an `IActorChangeEventListener`.
            Mod-A wants to ensure that ORefit is disabled for a specific actor,
            and so it disables ORefit for that actor in response to the events it handles.
            Whereas, Mod-B wants to ensure that ORefit is enabled for a grouping of actors,
            and the actor targeted by Mod-A falls within in the grouping,
            and so it enables ORefit for that actor in response to the events it handles.

            If OBody sent events recursively, what would happen is this:
                Mod-A would disable ORefit for the actor--triggering an `OnORefitForcefullyChanged` event--
                which Mod-B would receive and would thus then enable ORefit for the actor,
                which would then trigger another `OnORefitForcefullyChanged` event
                which Mod-A would react to. How would Mod-A react to the event?
                See the beginning of this paragraph for the answer.
                Mod-A and Mod-B would repeatedly reverse each other's changes
                until the game crashes due to a stack overflow.
                Before the crash, the game would be frozen.

                That would not be a sound basis for you to build your mod upon,
                hence why OBody does not send events recursively.

            As OBody does not send events recursively, and does not update the event arguments between
            event-listener calls, what actually happens is this:
                Mod-A will disable ORefit for the actor,
                and an `OnORefitForcefullyChanged` event won't be sent,
                Mod-B will observe that, according to the `flags` arguments, ORefit is enabled
                for the actor, and thus will do nothing.
                Or, it will call `IsORefitEnabled` on the actor, and find that ORefit is disabled,
                and so it would enable ORefit for the actor, and no `OnORefitForcefullyChanged` event will be sent.

                Note how the game does not freeze, nor does it crash.

            To clarify how this behaves when multiple actors are involved,
            say that we have one `IActorChangeEventListener`, and actors A, and B:
                The listener receives an event for actor-A, and makes a change to actor-A;
                the listener does not receive extra events for actor-A until all the listeners
                have handled actor-A.
                In the same event the listener makes a change to actor-B, the listener does receive
                an event for that change to actor-B, but if it makes further changes to actor-B or to actor-A
                in response to that event, no extra events will be sent.
                And so-on and so-on for any other actors.
    */
    class IActorChangeEventListener {
    public:
        /** The OnActorGenerated event is sent just after the assignment of a preset to an actor,
            and after OBody has either: applied the preset's morphs to the actor;
            or queued those morphs to be applied to the actor.
            (That is to say, the morphs may or not be visible to the player when you receive this event).

            This event is not sent when an actor's preset is reassigned but the actor is not regenerated.
            See the `OnActorPresetChangedWithoutGeneration` event for that scenario.
        */
        struct OnActorGenerated {
            struct Payload {
                /** This will be null if OBody itself was responsible for this event being fired.
                    Otherwise, this is the `IPluginInterface` that was responsible for this event being triggered.

                    There is a special `IPluginInterface` instance which OBody will set in this field
                    if this event was effected by OBody's Papyrus functions (the OBodyNative script).
                    You can identify that instance by its `owner` string, which is: "_Papyrus".

                    You can use this field to avoid acting upon changes that your own plugin effected,
                    or to avoid stepping on the toes of other mods that are making changes.*/
                IPluginInterfaceVersionIndependent* responsiblePluginInterface;

                /** The name of the BodySlide preset that was assigned to the actor.
                    Note that this is the name of the BodySlide preset as defined within the XML
                    of the BodySlide slider presets file, and not the name of the slider presets file itself.

                    This is a `string_view`, but the string it points to is null-terminated,
                    so it can be used as C-style string.
                    The data that this `string_view` points to is guaranteed to be valid
                    only until the event-listener's method returns.

                    For the `OnActorGenerated` event specifically, this is guaranteed to be non-null and not empty,
                    for other events this may be null if the actor has no preset applied to them. */
                const std::string_view presetName;
            };

            enum Flags : uint64_t {
                None = 0,
                /** This bit will be set if OBody considers the actor to be clothed. Elsewise the actor is naked. */
                IsClothed = 1 << 0,
                /** This bit will be set if ORefit is currently applied to the actor. */
                IsORefitApplied = 1 << 1,
                /** This bit will be set if ORefit is globally enabled for OBody. */
                IsORefitEnabled = 1 << 2
            };

            enum class Response : uint64_t {
                /** The default response: nothing special happens if you return it. */
                None = 0
            };
        };

        /** OBody will call this method to notify the listener of `OnActorGenerated` events. */
        virtual OnActorGenerated::Response OnActorGenerated([[maybe_unused]] Actor* actor,
                                                            [[maybe_unused]] OnActorGenerated::Flags flags,
                                                            [[maybe_unused]] OnActorGenerated::Payload& payload) {
            return OnActorGenerated::Response::None;
        }

        /** The OnActorPresetChangedWithoutGeneration event is sent just after the assignment of a preset
            to an actor, if the actor is not also being regenerated.
            This event is also sent when a preset is unassigned from an actor.
        */
        struct OnActorPresetChangedWithoutGeneration {
            struct Payload {
                /** Refer to the documentation of `responsiblePluginInterface` for the `OnActorGenerated` event. */
                IPluginInterfaceVersionIndependent* responsiblePluginInterface;

                /** The name of the BodySlide preset that was assigned to the actor.
                    Note that this is the name of the BodySlide preset as defined within the XML
                    of the BodySlide slider presets file, and not the name of the slider presets file itself.

                    This is a `string_view`, but the string it points to is null-terminated,
                    so it can be used as C-style string.
                    The data that this `string_view` points to is guaranteed to be valid
                    only until the event-listener's method returns.

                    If this string is null or empty,
                    it means that a preset has been unassigned from the actor
                    and the actor did not previously have a preset assigned to them. */
                const std::string_view presetName;
            };

            enum Flags : uint64_t {
                None = 0,
                /** If this bit is set, this event signals that a preset was unassigned from the actor.
                    And that the `presetName` field contains the name of the preset that the actor had
                    before it was unassigned. */
                PresetWasUnassigned = 1 << 0
            };

            enum class Response : uint64_t {
                /** The default response: nothing special happens if you return it. */
                None = 0
            };
        };

        /** OBody will call this method to notify the listener of `OnActorPresetChangedWithoutGeneration` events. */
        virtual OnActorPresetChangedWithoutGeneration::Response OnActorPresetChangedWithoutGeneration(
            [[maybe_unused]] Actor* actor, [[maybe_unused]] OnActorPresetChangedWithoutGeneration::Flags flags,
            [[maybe_unused]] OnActorPresetChangedWithoutGeneration::Payload& payload) {
            return OnActorPresetChangedWithoutGeneration::Response::None;
        }

        /** The OnActorClothingUpdate event is sent when the state of an actor's equipped clothing/armour changes.
            This event allows a listener to keep up-to-date on regardless of whether ORefit is active on actors,
            and regardless of whether OBody considers an actor to be naked or clothed.

            Note that internally this event is called from within the context of a `TESEquipEvent` event sink,
            and thus if the listener is querying the worn equipment of the actor, it may need to consider the
            equipment that is being [un]equipped by the actor, which can be accessed in the payload of this event.
            (See also `IPluginInterface::ActorIsNaked`).
        */
        struct OnActorClothingUpdate {
            struct Payload {
                /** Refer to the documentation of `responsiblePluginInterface` for the `OnActorGenerated` event. */
                IPluginInterfaceVersionIndependent* responsiblePluginInterface;

                /** The equipment that is being equipped or unequipped by the actor,
                    check the flags for which it is. This will not be null. */
                const TESForm* changedEquipment;
            };

            enum Flags : uint64_t {
                None = 0,
                /** This bit will be set if OBody considers the actor to be clothed. Elsewise the actor is naked. */
                IsClothed = 1 << 0,
                /** This bit will be set if ORefit is currently applied to the actor. */
                IsORefitApplied = 1 << 1,
                /** This bit will be set if ORefit is globally enabled for OBody. */
                IsORefitEnabled = 1 << 2,
                /** This bit will be set if OBody considers the actor to be processed.
                    (See `IPluginInterface::ActorIsProcessed`). */
                IsProcessed = 1 << 3,
                /** This bit will be set if OBody considers the actor to be blacklisted.
                    (See `IPluginInterface::ActorIsBlacklisted`). */
                IsBlacklisted = 1 << 4,
                /** This bit will be set if the actor is equipping equipment, otherwise the actor is unequipping. */
                ActorIsEquipping = 1 << 5
            };

            enum class Response : uint64_t {
                /** The default response: nothing special happens if you return it. */
                None = 0
            };
        };

        /** OBody will call this method to notify the listener of `OnActorClothingUpdate` events. */
        virtual OnActorClothingUpdate::Response OnActorClothingUpdate(
            [[maybe_unused]] Actor* actor, [[maybe_unused]] OnActorClothingUpdate::Flags flags,
            [[maybe_unused]] OnActorClothingUpdate::Payload& payload) {
            return OnActorClothingUpdate::Response::None;
        }

        /** The OnORefitForcefullyChanged event is sent when ORefit is forcefully enabled or disabled
            for an actor; typically as the result of a Papyrus script calling
            `OBodyNative.AddClothesOverlay` or `OBodyNative.RemoveClothesOverlay`.
        */
        struct OnORefitForcefullyChanged {
            struct Payload {
                /** Refer to the documentation of `responsiblePluginInterface` for the `OnActorGenerated` event. */
                IPluginInterfaceVersionIndependent* responsiblePluginInterface;
            };

            enum Flags : uint64_t {
                None = 0,
                /** This bit will be set if ORefit is currently applied to the actor. */
                IsORefitApplied = 1 << 1,
                /** This bit will be set if ORefit is globally enabled for OBody. */
                IsORefitEnabled = 1 << 2
            };

            enum class Response : uint64_t {
                /** The default response: nothing special happens if you return it. */
                None = 0
            };
        };

        /** OBody will call this method to notify the listener of `OnORefitForcefullyChanged` events. */
        virtual OnORefitForcefullyChanged::Response OnORefitForcefullyChanged(
            [[maybe_unused]] Actor* actor, [[maybe_unused]] OnORefitForcefullyChanged::Flags flags,
            [[maybe_unused]] OnORefitForcefullyChanged::Payload& payload) {
            return OnORefitForcefullyChanged::Response::None;
        }

        /** The OnActorMorphsCleared event is sent when an actor's OBody morphs are cleared;
            typically as the result of a Papyrus script calling `OBodyNative.ResetActorOBodyMorphs`.
            (Implicitly, this means that ORefit is not active for the actor).
        */
        struct OnActorMorphsCleared {
            struct Payload {
                /** Refer to the documentation of `responsiblePluginInterface` for the `OnActorGenerated` event. */
                IPluginInterfaceVersionIndependent* responsiblePluginInterface;
            };

            enum Flags : uint64_t { None = 0 };

            enum class Response : uint64_t {
                /** The default response: nothing special happens if you return it. */
                None = 0
            };
        };

        /** OBody will call this method to notify the listener of `OnActorMorphsCleared` events. */
        virtual OnActorMorphsCleared::Response OnActorMorphsCleared(
            [[maybe_unused]] Actor* actor, [[maybe_unused]] OnActorMorphsCleared::Flags flags,
            [[maybe_unused]] OnActorMorphsCleared::Payload& payload) {
            return OnActorMorphsCleared::Response::None;
        }
    };

    /** These structures are to be used to send messages to OBody via SKSE's messaging interface.
        Their general usage is such that you allocate the structure somewhere--likely on the stack--
        and the structure's address is then used for the message's `data` pointer, and the `sizeof`
        of the structure is used for the message's `dataLen`.

        See the top of this header for an example involving the `RequestPluginInterface` message.
    */
    namespace SKSEMessages {
        /** The `RequestPluginInterface` message is used to request an `IPluginInterface` instance from OBody,
            thus this can be thought of as the entry-point to OBody's plugin-API.

            Before sending this message, you must set the `version` field to the version of the plugin API
            that your SKSE plugin supports; this allows OBody to return a different `IPluginInterface` instance
            to your plugin according to that version, which permits OBody to update and alter its API without
            breaking backwards compatibility with your already-compiled mod.

            Secondly, you must supply a valid pointer to an `IOBodyReadinessEventListener` instance
            via the `readinessEventListener` field.
            The `IOBodyReadinessEventListener` instance pointed-to by this field must remain valid
            until the process terminates.

            In response to this message, OBody will write through the pointer of the `pluginInterface` field.
            If your message was valid and OBody can satisfy it, the pointed-to `pluginInterface` will be a pointer
            to a valid `IPluginInterface` instance.
            Otherwise, if your message was invalid or could not be satisfied, such as if you requested a version
            that is not a valid `PluginAPIVersion` value, or OBody has stopped supporting your requested version,
            then `pluginInterface` will not be written through.
            Likewise, if you failed to supply an `IOBodyReadinessEventListener`.
            If the `dataLen` value you send is smaller than three pointers,
            then OBody will not respond to the message.

            The `IPluginInterface` instance you receive is not safe to use
            until the `IOBodyReadinessEventListener` instance you supplied receives an `OBodyIsReady` event.
            See the documentation for `IOBodyReadinessEventListener` for more detail.

            Whilst the handler that receives this message is thread-safe,
            you MUST not send this message to OBody from within the context
            of an `IOBodyReadinessEventListener` method called by OBody,
            because doing so will invalidate the iterator that OBody is acting upon.

            The reason why `pluginInterface` is a pointer to a pointer that is written through,
            instead of simply returning a pointer via the message, is so that the `IPluginInterface*`
            can be written directly to a location accessible by your `IOBodyReadinessEventListener` instance.
            This is important as the `OBodyIsReady` event can be sent before you receive a response for the message.
        */
        struct RequestPluginInterface {
            /** The value for the `type` of the SKSE message. */
            static constexpr uint32_t type = 0xc0B0D9cc;

            /** The version of the plugin that you support; see above. (You send this). */
            PluginAPIVersion version;

            /** A pointer to a pointer to an `IPluginInterface` instance; see above. (You send this).
                The `IPluginInterface*` that is written through this pointer will have been allocated by `new`. */
            IPluginInterface** pluginInterface;

            /** A pointer to an `IOBodyReadinessEventListener` instance; see above. (You send this). */
            IOBodyReadinessEventListener* readinessEventListener;
        };
    }  // namespace SKSEMessages
}  // namespace OBody::API

/*
GNU GENERAL PUBLIC LICENSE
                       Version 3, 29 June 2007

 Copyright (C) 2007 Free Software Foundation, Inc. <https://fsf.org/>
 Everyone is permitted to copy and distribute verbatim copies
 of this license document, but changing it is not allowed.

                            Preamble

  The GNU General Public License is a free, copyleft license for
software and other kinds of works.

  The licenses for most software and other practical works are designed
to take away your freedom to share and change the works.  By contrast,
the GNU General Public License is intended to guarantee your freedom to
share and change all versions of a program--to make sure it remains free
software for all its users.  We, the Free Software Foundation, use the
GNU General Public License for most of our software; it applies also to
any other work released this way by its authors.  You can apply it to
your programs, too.

  When we speak of free software, we are referring to freedom, not
price.  Our General Public Licenses are designed to make sure that you
have the freedom to distribute copies of free software (and charge for
them if you wish), that you receive source code or can get it if you
want it, that you can change the software or use pieces of it in new
free programs, and that you know you can do these things.

  To protect your rights, we need to prevent others from denying you
these rights or asking you to surrender the rights.  Therefore, you have
certain responsibilities if you distribute copies of the software, or if
you modify it: responsibilities to respect the freedom of others.

  For example, if you distribute copies of such a program, whether
gratis or for a fee, you must pass on to the recipients the same
freedoms that you received.  You must make sure that they, too, receive
or can get the source code.  And you must show them these terms so they
know their rights.

  Developers that use the GNU GPL protect your rights with two steps:
(1) assert copyright on the software, and (2) offer you this License
giving you legal permission to copy, distribute and/or modify it.

  For the developers' and authors' protection, the GPL clearly explains
that there is no warranty for this free software.  For both users' and
authors' sake, the GPL requires that modified versions be marked as
changed, so that their problems will not be attributed erroneously to
authors of previous versions.

  Some devices are designed to deny users access to install or run
modified versions of the software inside them, although the manufacturer
can do so.  This is fundamentally incompatible with the aim of
protecting users' freedom to change the software.  The systematic
pattern of such abuse occurs in the area of products for individuals to
use, which is precisely where it is most unacceptable.  Therefore, we
have designed this version of the GPL to prohibit the practice for those
products.  If such problems arise substantially in other domains, we
stand ready to extend this provision to those domains in future versions
of the GPL, as needed to protect the freedom of users.

  Finally, every program is threatened constantly by software patents.
States should not allow patents to restrict development and use of
software on general-purpose computers, but in those that do, we wish to
avoid the special danger that patents applied to a free program could
make it effectively proprietary.  To prevent this, the GPL assures that
patents cannot be used to render the program non-free.

  The precise terms and conditions for copying, distribution and
modification follow.

                       TERMS AND CONDITIONS

  0. Definitions.

  "This License" refers to version 3 of the GNU General Public License.

  "Copyright" also means copyright-like laws that apply to other kinds of
works, such as semiconductor masks.

  "The Program" refers to any copyrightable work licensed under this
License.  Each licensee is addressed as "you".  "Licensees" and
"recipients" may be individuals or organizations.

  To "modify" a work means to copy from or adapt all or part of the work
in a fashion requiring copyright permission, other than the making of an
exact copy.  The resulting work is called a "modified version" of the
earlier work or a work "based on" the earlier work.

  A "covered work" means either the unmodified Program or a work based
on the Program.

  To "propagate" a work means to do anything with it that, without
permission, would make you directly or secondarily liable for
infringement under applicable copyright law, except executing it on a
computer or modifying a private copy.  Propagation includes copying,
distribution (with or without modification), making available to the
public, and in some countries other activities as well.

  To "convey" a work means any kind of propagation that enables other
parties to make or receive copies.  Mere interaction with a user through
a computer network, with no transfer of a copy, is not conveying.

  An interactive user interface displays "Appropriate Legal Notices"
to the extent that it includes a convenient and prominently visible
feature that (1) displays an appropriate copyright notice, and (2)
tells the user that there is no warranty for the work (except to the
extent that warranties are provided), that licensees may convey the
work under this License, and how to view a copy of this License.  If
the interface presents a list of user commands or options, such as a
menu, a prominent item in the list meets this criterion.

  1. Source Code.

  The "source code" for a work means the preferred form of the work
for making modifications to it.  "Object code" means any non-source
form of a work.

  A "Standard Interface" means an interface that either is an official
standard defined by a recognized standards body, or, in the case of
interfaces specified for a particular programming language, one that
is widely used among developers working in that language.

  The "System Libraries" of an executable work include anything, other
than the work as a whole, that (a) is included in the normal form of
packaging a Major Component, but which is not part of that Major
Component, and (b) serves only to enable use of the work with that
Major Component, or to implement a Standard Interface for which an
implementation is available to the public in source code form.  A
"Major Component", in this context, means a major essential component
(kernel, window system, and so on) of the specific operating system
(if any) on which the executable work runs, or a compiler used to
produce the work, or an object code interpreter used to run it.

  The "Corresponding Source" for a work in object code form means all
the source code needed to generate, install, and (for an executable
work) run the object code and to modify the work, including scripts to
control those activities.  However, it does not include the work's
System Libraries, or general-purpose tools or generally available free
programs which are used unmodified in performing those activities but
which are not part of the work.  For example, Corresponding Source
includes interface definition files associated with source files for
the work, and the source code for shared libraries and dynamically
linked subprograms that the work is specifically designed to require,
such as by intimate data communication or control flow between those
subprograms and other parts of the work.

  The Corresponding Source need not include anything that users
can regenerate automatically from other parts of the Corresponding
Source.

  The Corresponding Source for a work in source code form is that
same work.

  2. Basic Permissions.

  All rights granted under this License are granted for the term of
copyright on the Program, and are irrevocable provided the stated
conditions are met.  This License explicitly affirms your unlimited
permission to run the unmodified Program.  The output from running a
covered work is covered by this License only if the output, given its
content, constitutes a covered work.  This License acknowledges your
rights of fair use or other equivalent, as provided by copyright law.

  You may make, run and propagate covered works that you do not
convey, without conditions so long as your license otherwise remains
in force.  You may convey covered works to others for the sole purpose
of having them make modifications exclusively for you, or provide you
with facilities for running those works, provided that you comply with
the terms of this License in conveying all material for which you do
not control copyright.  Those thus making or running the covered works
for you must do so exclusively on your behalf, under your direction
and control, on terms that prohibit them from making any copies of
your copyrighted material outside their relationship with you.

  Conveying under any other circumstances is permitted solely under
the conditions stated below.  Sublicensing is not allowed; section 10
makes it unnecessary.

  3. Protecting Users' Legal Rights From Anti-Circumvention Law.

  No covered work shall be deemed part of an effective technological
measure under any applicable law fulfilling obligations under article
11 of the WIPO copyright treaty adopted on 20 December 1996, or
similar laws prohibiting or restricting circumvention of such
measures.

  When you convey a covered work, you waive any legal power to forbid
circumvention of technological measures to the extent such circumvention
is effected by exercising rights under this License with respect to
the covered work, and you disclaim any intention to limit operation or
modification of the work as a means of enforcing, against the work's
users, your or third parties' legal rights to forbid circumvention of
technological measures.

  4. Conveying Verbatim Copies.

  You may convey verbatim copies of the Program's source code as you
receive it, in any medium, provided that you conspicuously and
appropriately publish on each copy an appropriate copyright notice;
keep intact all notices stating that this License and any
non-permissive terms added in accord with section 7 apply to the code;
keep intact all notices of the absence of any warranty; and give all
recipients a copy of this License along with the Program.

  You may charge any price or no price for each copy that you convey,
and you may offer support or warranty protection for a fee.

  5. Conveying Modified Source Versions.

  You may convey a work based on the Program, or the modifications to
produce it from the Program, in the form of source code under the
terms of section 4, provided that you also meet all of these conditions:

    a) The work must carry prominent notices stating that you modified
    it, and giving a relevant date.

    b) The work must carry prominent notices stating that it is
    released under this License and any conditions added under section
    7.  This requirement modifies the requirement in section 4 to
    "keep intact all notices".

    c) You must license the entire work, as a whole, under this
    License to anyone who comes into possession of a copy.  This
    License will therefore apply, along with any applicable section 7
    additional terms, to the whole of the work, and all its parts,
    regardless of how they are packaged.  This License gives no
    permission to license the work in any other way, but it does not
    invalidate such permission if you have separately received it.

    d) If the work has interactive user interfaces, each must display
    Appropriate Legal Notices; however, if the Program has interactive
    interfaces that do not display Appropriate Legal Notices, your
    work need not make them do so.

  A compilation of a covered work with other separate and independent
works, which are not by their nature extensions of the covered work,
and which are not combined with it such as to form a larger program,
in or on a volume of a storage or distribution medium, is called an
"aggregate" if the compilation and its resulting copyright are not
used to limit the access or legal rights of the compilation's users
beyond what the individual works permit.  Inclusion of a covered work
in an aggregate does not cause this License to apply to the other
parts of the aggregate.

  6. Conveying Non-Source Forms.

  You may convey a covered work in object code form under the terms
of sections 4 and 5, provided that you also convey the
machine-readable Corresponding Source under the terms of this License,
in one of these ways:

    a) Convey the object code in, or embodied in, a physical product
    (including a physical distribution medium), accompanied by the
    Corresponding Source fixed on a durable physical medium
    customarily used for software interchange.

    b) Convey the object code in, or embodied in, a physical product
    (including a physical distribution medium), accompanied by a
    written offer, valid for at least three years and valid for as
    long as you offer spare parts or customer support for that product
    model, to give anyone who possesses the object code either (1) a
    copy of the Corresponding Source for all the software in the
    product that is covered by this License, on a durable physical
    medium customarily used for software interchange, for a price no
    more than your reasonable cost of physically performing this
    conveying of source, or (2) access to copy the
    Corresponding Source from a network server at no charge.

    c) Convey individual copies of the object code with a copy of the
    written offer to provide the Corresponding Source.  This
    alternative is allowed only occasionally and noncommercially, and
    only if you received the object code with such an offer, in accord
    with subsection 6b.

    d) Convey the object code by offering access from a designated
    place (gratis or for a charge), and offer equivalent access to the
    Corresponding Source in the same way through the same place at no
    further charge.  You need not require recipients to copy the
    Corresponding Source along with the object code.  If the place to
    copy the object code is a network server, the Corresponding Source
    may be on a different server (operated by you or a third party)
    that supports equivalent copying facilities, provided you maintain
    clear directions next to the object code saying where to find the
    Corresponding Source.  Regardless of what server hosts the
    Corresponding Source, you remain obligated to ensure that it is
    available for as long as needed to satisfy these requirements.

    e) Convey the object code using peer-to-peer transmission, provided
    you inform other peers where the object code and Corresponding
    Source of the work are being offered to the general public at no
    charge under subsection 6d.

  A separable portion of the object code, whose source code is excluded
from the Corresponding Source as a System Library, need not be
included in conveying the object code work.

  A "User Product" is either (1) a "consumer product", which means any
tangible personal property which is normally used for personal, family,
or household purposes, or (2) anything designed or sold for incorporation
into a dwelling.  In determining whether a product is a consumer product,
doubtful cases shall be resolved in favor of coverage.  For a particular
product received by a particular user, "normally used" refers to a
typical or common use of that class of product, regardless of the status
of the particular user or of the way in which the particular user
actually uses, or expects or is expected to use, the product.  A product
is a consumer product regardless of whether the product has substantial
commercial, industrial or non-consumer uses, unless such uses represent
the only significant mode of use of the product.

  "Installation Information" for a User Product means any methods,
procedures, authorization keys, or other information required to install
and execute modified versions of a covered work in that User Product from
a modified version of its Corresponding Source.  The information must
suffice to ensure that the continued functioning of the modified object
code is in no case prevented or interfered with solely because
modification has been made.

  If you convey an object code work under this section in, or with, or
specifically for use in, a User Product, and the conveying occurs as
part of a transaction in which the right of possession and use of the
User Product is transferred to the recipient in perpetuity or for a
fixed term (regardless of how the transaction is characterized), the
Corresponding Source conveyed under this section must be accompanied
by the Installation Information.  But this requirement does not apply
if neither you nor any third party retains the ability to install
modified object code on the User Product (for example, the work has
been installed in ROM).

  The requirement to provide Installation Information does not include a
requirement to continue to provide support service, warranty, or updates
for a work that has been modified or installed by the recipient, or for
the User Product in which it has been modified or installed.  Access to a
network may be denied when the modification itself materially and
adversely affects the operation of the network or violates the rules and
protocols for communication across the network.

  Corresponding Source conveyed, and Installation Information provided,
in accord with this section must be in a format that is publicly
documented (and with an implementation available to the public in
source code form), and must require no special password or key for
unpacking, reading or copying.

  7. Additional Terms.

  "Additional permissions" are terms that supplement the terms of this
License by making exceptions from one or more of its conditions.
Additional permissions that are applicable to the entire Program shall
be treated as though they were included in this License, to the extent
that they are valid under applicable law.  If additional permissions
apply only to part of the Program, that part may be used separately
under those permissions, but the entire Program remains governed by
this License without regard to the additional permissions.

  When you convey a copy of a covered work, you may at your option
remove any additional permissions from that copy, or from any part of
it.  (Additional permissions may be written to require their own
removal in certain cases when you modify the work.)  You may place
additional permissions on material, added by you to a covered work,
for which you have or can give appropriate copyright permission.

  Notwithstanding any other provision of this License, for material you
add to a covered work, you may (if authorized by the copyright holders of
that material) supplement the terms of this License with terms:

    a) Disclaiming warranty or limiting liability differently from the
    terms of sections 15 and 16 of this License; or

    b) Requiring preservation of specified reasonable legal notices or
    author attributions in that material or in the Appropriate Legal
    Notices displayed by works containing it; or

    c) Prohibiting misrepresentation of the origin of that material, or
    requiring that modified versions of such material be marked in
    reasonable ways as different from the original version; or

    d) Limiting the use for publicity purposes of names of licensors or
    authors of the material; or

    e) Declining to grant rights under trademark law for use of some
    trade names, trademarks, or service marks; or

    f) Requiring indemnification of licensors and authors of that
    material by anyone who conveys the material (or modified versions of
    it) with contractual assumptions of liability to the recipient, for
    any liability that these contractual assumptions directly impose on
    those licensors and authors.

  All other non-permissive additional terms are considered "further
restrictions" within the meaning of section 10.  If the Program as you
received it, or any part of it, contains a notice stating that it is
governed by this License along with a term that is a further
restriction, you may remove that term.  If a license document contains
a further restriction but permits relicensing or conveying under this
License, you may add to a covered work material governed by the terms
of that license document, provided that the further restriction does
not survive such relicensing or conveying.

  If you add terms to a covered work in accord with this section, you
must place, in the relevant source files, a statement of the
additional terms that apply to those files, or a notice indicating
where to find the applicable terms.

  Additional terms, permissive or non-permissive, may be stated in the
form of a separately written license, or stated as exceptions;
the above requirements apply either way.

  8. Termination.

  You may not propagate or modify a covered work except as expressly
provided under this License.  Any attempt otherwise to propagate or
modify it is void, and will automatically terminate your rights under
this License (including any patent licenses granted under the third
paragraph of section 11).

  However, if you cease all violation of this License, then your
license from a particular copyright holder is reinstated (a)
provisionally, unless and until the copyright holder explicitly and
finally terminates your license, and (b) permanently, if the copyright
holder fails to notify you of the violation by some reasonable means
prior to 60 days after the cessation.

  Moreover, your license from a particular copyright holder is
reinstated permanently if the copyright holder notifies you of the
violation by some reasonable means, this is the first time you have
received notice of violation of this License (for any work) from that
copyright holder, and you cure the violation prior to 30 days after
your receipt of the notice.

  Termination of your rights under this section does not terminate the
licenses of parties who have received copies or rights from you under
this License.  If your rights have been terminated and not permanently
reinstated, you do not qualify to receive new licenses for the same
material under section 10.

  9. Acceptance Not Required for Having Copies.

  You are not required to accept this License in order to receive or
run a copy of the Program.  Ancillary propagation of a covered work
occurring solely as a consequence of using peer-to-peer transmission
to receive a copy likewise does not require acceptance.  However,
nothing other than this License grants you permission to propagate or
modify any covered work.  These actions infringe copyright if you do
not accept this License.  Therefore, by modifying or propagating a
covered work, you indicate your acceptance of this License to do so.

  10. Automatic Licensing of Downstream Recipients.

  Each time you convey a covered work, the recipient automatically
receives a license from the original licensors, to run, modify and
propagate that work, subject to this License.  You are not responsible
for enforcing compliance by third parties with this License.

  An "entity transaction" is a transaction transferring control of an
organization, or substantially all assets of one, or subdividing an
organization, or merging organizations.  If propagation of a covered
work results from an entity transaction, each party to that
transaction who receives a copy of the work also receives whatever
licenses to the work the party's predecessor in interest had or could
give under the previous paragraph, plus a right to possession of the
Corresponding Source of the work from the predecessor in interest, if
the predecessor has it or can get it with reasonable efforts.

  You may not impose any further restrictions on the exercise of the
rights granted or affirmed under this License.  For example, you may
not impose a license fee, royalty, or other charge for exercise of
rights granted under this License, and you may not initiate litigation
(including a cross-claim or counterclaim in a lawsuit) alleging that
any patent claim is infringed by making, using, selling, offering for
sale, or importing the Program or any portion of it.

  11. Patents.

  A "contributor" is a copyright holder who authorizes use under this
License of the Program or a work on which the Program is based.  The
work thus licensed is called the contributor's "contributor version".

  A contributor's "essential patent claims" are all patent claims
owned or controlled by the contributor, whether already acquired or
hereafter acquired, that would be infringed by some manner, permitted
by this License, of making, using, or selling its contributor version,
but do not include claims that would be infringed only as a
consequence of further modification of the contributor version.  For
purposes of this definition, "control" includes the right to grant
patent sublicenses in a manner consistent with the requirements of
this License.

  Each contributor grants you a non-exclusive, worldwide, royalty-free
patent license under the contributor's essential patent claims, to
make, use, sell, offer for sale, import and otherwise run, modify and
propagate the contents of its contributor version.

  In the following three paragraphs, a "patent license" is any express
agreement or commitment, however denominated, not to enforce a patent
(such as an express permission to practice a patent or covenant not to
sue for patent infringement).  To "grant" such a patent license to a
party means to make such an agreement or commitment not to enforce a
patent against the party.

  If you convey a covered work, knowingly relying on a patent license,
and the Corresponding Source of the work is not available for anyone
to copy, free of charge and under the terms of this License, through a
publicly available network server or other readily accessible means,
then you must either (1) cause the Corresponding Source to be so
available, or (2) arrange to deprive yourself of the benefit of the
patent license for this particular work, or (3) arrange, in a manner
consistent with the requirements of this License, to extend the patent
license to downstream recipients.  "Knowingly relying" means you have
actual knowledge that, but for the patent license, your conveying the
covered work in a country, or your recipient's use of the covered work
in a country, would infringe one or more identifiable patents in that
country that you have reason to believe are valid.

  If, pursuant to or in connection with a single transaction or
arrangement, you convey, or propagate by procuring conveyance of, a
covered work, and grant a patent license to some of the parties
receiving the covered work authorizing them to use, propagate, modify
or convey a specific copy of the covered work, then the patent license
you grant is automatically extended to all recipients of the covered
work and works based on it.

  A patent license is "discriminatory" if it does not include within
the scope of its coverage, prohibits the exercise of, or is
conditioned on the non-exercise of one or more of the rights that are
specifically granted under this License.  You may not convey a covered
work if you are a party to an arrangement with a third party that is
in the business of distributing software, under which you make payment
to the third party based on the extent of your activity of conveying
the work, and under which the third party grants, to any of the
parties who would receive the covered work from you, a discriminatory
patent license (a) in connection with copies of the covered work
conveyed by you (or copies made from those copies), or (b) primarily
for and in connection with specific products or compilations that
contain the covered work, unless you entered into that arrangement,
or that patent license was granted, prior to 28 March 2007.

  Nothing in this License shall be construed as excluding or limiting
any implied license or other defenses to infringement that may
otherwise be available to you under applicable patent law.

  12. No Surrender of Others' Freedom.

  If conditions are imposed on you (whether by court order, agreement or
otherwise) that contradict the conditions of this License, they do not
excuse you from the conditions of this License.  If you cannot convey a
covered work so as to satisfy simultaneously your obligations under this
License and any other pertinent obligations, then as a consequence you may
not convey it at all.  For example, if you agree to terms that obligate you
to collect a royalty for further conveying from those to whom you convey
the Program, the only way you could satisfy both those terms and this
License would be to refrain entirely from conveying the Program.

  13. Use with the GNU Affero General Public License.

  Notwithstanding any other provision of this License, you have
permission to link or combine any covered work with a work licensed
under version 3 of the GNU Affero General Public License into a single
combined work, and to convey the resulting work.  The terms of this
License will continue to apply to the part which is the covered work,
but the special requirements of the GNU Affero General Public License,
section 13, concerning interaction through a network will apply to the
combination as such.

  14. Revised Versions of this License.

  The Free Software Foundation may publish revised and/or new versions of
the GNU General Public License from time to time.  Such new versions will
be similar in spirit to the present version, but may differ in detail to
address new problems or concerns.

  Each version is given a distinguishing version number.  If the
Program specifies that a certain numbered version of the GNU General
Public License "or any later version" applies to it, you have the
option of following the terms and conditions either of that numbered
version or of any later version published by the Free Software
Foundation.  If the Program does not specify a version number of the
GNU General Public License, you may choose any version ever published
by the Free Software Foundation.

  If the Program specifies that a proxy can decide which future
versions of the GNU General Public License can be used, that proxy's
public statement of acceptance of a version permanently authorizes you
to choose that version for the Program.

  Later license versions may give you additional or different
permissions.  However, no additional obligations are imposed on any
author or copyright holder as a result of your choosing to follow a
later version.

  15. Disclaimer of Warranty.

  THERE IS NO WARRANTY FOR THE PROGRAM, TO THE EXTENT PERMITTED BY
APPLICABLE LAW.  EXCEPT WHEN OTHERWISE STATED IN WRITING THE COPYRIGHT
HOLDERS AND/OR OTHER PARTIES PROVIDE THE PROGRAM "AS IS" WITHOUT WARRANTY
OF ANY KIND, EITHER EXPRESSED OR IMPLIED, INCLUDING, BUT NOT LIMITED TO,
THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR
PURPOSE.  THE ENTIRE RISK AS TO THE QUALITY AND PERFORMANCE OF THE PROGRAM
IS WITH YOU.  SHOULD THE PROGRAM PROVE DEFECTIVE, YOU ASSUME THE COST OF
ALL NECESSARY SERVICING, REPAIR OR CORRECTION.

  16. Limitation of Liability.

  IN NO EVENT UNLESS REQUIRED BY APPLICABLE LAW OR AGREED TO IN WRITING
WILL ANY COPYRIGHT HOLDER, OR ANY OTHER PARTY WHO MODIFIES AND/OR CONVEYS
THE PROGRAM AS PERMITTED ABOVE, BE LIABLE TO YOU FOR DAMAGES, INCLUDING ANY
GENERAL, SPECIAL, INCIDENTAL OR CONSEQUENTIAL DAMAGES ARISING OUT OF THE
USE OR INABILITY TO USE THE PROGRAM (INCLUDING BUT NOT LIMITED TO LOSS OF
DATA OR DATA BEING RENDERED INACCURATE OR LOSSES SUSTAINED BY YOU OR THIRD
PARTIES OR A FAILURE OF THE PROGRAM TO OPERATE WITH ANY OTHER PROGRAMS),
EVEN IF SUCH HOLDER OR OTHER PARTY HAS BEEN ADVISED OF THE POSSIBILITY OF
SUCH DAMAGES.

  17. Interpretation of Sections 15 and 16.

  If the disclaimer of warranty and limitation of liability provided
above cannot be given local legal effect according to their terms,
reviewing courts shall apply local law that most closely approximates
an absolute waiver of all civil liability in connection with the
Program, unless a warranty or assumption of liability accompanies a
copy of the Program in return for a fee.

                     END OF TERMS AND CONDITIONS

            How to Apply These Terms to Your New Programs

  If you develop a new program, and you want it to be of the greatest
possible use to the public, the best way to achieve this is to make it
free software which everyone can redistribute and change under these terms.

  To do so, attach the following notices to the program.  It is safest
to attach them to the start of each source file to most effectively
state the exclusion of warranty; and each file should have at least
the "copyright" line and a pointer to where the full notice is found.

    <one line to give the program's name and a brief idea of what it does.>
    Copyright (C) <year>  <name of author>

    This program is free software: you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation, either version 3 of the License, or
    (at your option) any later version.

    This program is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU General Public License for more details.

    You should have received a copy of the GNU General Public License
    along with this program.  If not, see <https://www.gnu.org/licenses/>.

Also add information on how to contact you by electronic and paper mail.

  If the program does terminal interaction, make it output a short
notice like this when it starts in an interactive mode:

    <program>  Copyright (C) <year>  <name of author>
    This program comes with ABSOLUTELY NO WARRANTY; for details type `show w'.
    This is free software, and you are welcome to redistribute it
    under certain conditions; type `show c' for details.

The hypothetical commands `show w' and `show c' should show the appropriate
parts of the General Public License.  Of course, your program's commands
might be different; for a GUI interface, you would use an "about box".

  You should also get your employer (if you work as a programmer) or school,
if any, to sign a "copyright disclaimer" for the program, if necessary.
For more information on this, and how to apply and follow the GNU GPL, see
<https://www.gnu.org/licenses/>.

  The GNU General Public License does not permit incorporating your program
into proprietary programs.  If your program is a subroutine library, you
may consider it more useful to permit linking proprietary applications with
the library.  If this is what you want to do, use the GNU Lesser General
Public License instead of this License.  But first, please read
<https://www.gnu.org/licenses/why-not-lgpl.html>.
*/
