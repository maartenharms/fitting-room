#pragma once

#include <algorithm>
#include <cstddef>
#include <string>
#include <unordered_map>
#include <vector>

// Pure label-disambiguation logic for the editor's "Editing:" target selector
// (Task 8). Followers frequently share a display name (two "Guard"s, a modded
// "Lydia" replacer alongside vanilla Lydia); the picker appends the base
// plugin filename to a label ONLY when the bare name collides with another
// entry, so the common case stays clean ("Lydia") and the ambiguous case
// self-disambiguates ("Lydia (Bijin.esp)"). Header-only + engine-free so the
// rule is unit-tested without RE:: types (see tests/test_npcsession.cpp).
namespace OS {

    struct TargetLabelInput {
        std::string name;    // the actor/base display name
        std::string plugin;  // the base NPC's source plugin filename
    };

    // One label per input, index-aligned. An entry whose `name` appears more
    // than once across the inputs is rendered "name (plugin)"; a unique name is
    // rendered bare. An empty name falls back to the plugin alone (never an
    // empty label). Collision is counted on the trimmed display name only -
    // plugin is the tiebreaker, not part of the identity being matched.
    [[nodiscard]] inline std::vector<std::string> BuildDisambiguatedLabels(
        const std::vector<TargetLabelInput>& a_inputs) {
        std::unordered_map<std::string, int> nameCount;
        for (const auto& in : a_inputs) {
            ++nameCount[in.name];
        }
        std::vector<std::string> out;
        out.reserve(a_inputs.size());
        for (const auto& in : a_inputs) {
            if (in.name.empty()) {
                out.push_back(in.plugin.empty() ? std::string{ "?" } : in.plugin);
                continue;
            }
            if (nameCount[in.name] > 1 && !in.plugin.empty()) {
                out.push_back(in.name + " (" + in.plugin + ")");
            } else {
                out.push_back(in.name);
            }
        }
        return out;
    }

    // The roster walk hands people back in process-list order, which is
    // nobody's order (Trello, user 2026-08-22: "make sure npcs that are
    // closest to you appear first in the order of npcs to edit dropdown").
    // Distances arrive index-aligned with the entries; the return is the
    // order to visit them in, nearest first. stable_sort on purpose: equal
    // distances keep the walk's order, so two people on one spot never swap
    // between opens. Pure so the rule is unit-tested without RE:: types,
    // exactly like the labels above (tests/test_npcsession.cpp), and the
    // caller applies one permutation to every parallel array it keeps.
    [[nodiscard]] inline std::vector<std::size_t> NearestFirstOrder(
        const std::vector<float>& a_distances) {
        std::vector<std::size_t> order(a_distances.size());
        for (std::size_t i = 0; i < order.size(); ++i) {
            order[i] = i;
        }
        std::stable_sort(order.begin(), order.end(),
                         [&](std::size_t a_lhs, std::size_t a_rhs) {
                             return a_distances[a_lhs] < a_distances[a_rhs];
                         });
        return order;
    }

}  // namespace OS
