#include "DyeUnlockCard.h"

#include "DyePromotion.h"  // AnnouncesCards: the economy switch gates the cards

#include "DyeCardQueue.h"
#include "DyePalette.h"
#include "EditorStyle.h"  // PlayUISound, for the arrival cue
#include "EditorWindow.h"
#include "FuckCompat.h"
#include "Settings.h"

#include <algorithm>
#include <cmath>  // std::floor - the rules are a whole-number stroke
#include <atomic>
#include <mutex>

namespace OS::DyeUnlockCard {

    namespace {

        // ---- shared state ---------------------------------------------------
        //
        // ⚠ TWO THREADS, ONE QUEUE. Announce runs on the main thread out of the
        // promotion pass; Draw and IsOpen run on FUCK's present thread. The
        // queue itself is deliberately pure and lock-free so it can be tested
        // headless, so the lock lives here, around every touch of it.
        std::mutex        g_lock;
        DyeCardQueue      g_queue;
        std::atomic<bool> g_armed{ false };
        std::atomic<bool> g_enabled{ true };

        // ---- geometry, all in unscaled pixels ------------------------------
        //
        // ⚠ THE CARD IS FLUSH TO THE RIGHT SCREEN EDGE AND SLIDES OUT OF IT.
        // It used to float 24px in from the edge and slide 46px sideways inside
        // its own window. The reference the user gave is Vel'dun's TrueHUD loot
        // history, where the plate emerges FROM the margin, so the right edge is
        // the screen edge and the slide is a whole card width: the card starts
        // entirely past the screen edge, where the window's clip rect hides it,
        // and eases into place.
        //
        // ⚠ THE MOTION WAS KEPT AND THE SHAPE WAS NOT. The same reference also
        // has a cut-corner plate, and that half was tried and rejected on sight
        // (see DrawCard). Emerging from the margin is what the field asked for;
        // the chamfer is what it called cheap.
        //
        // ⚠⚠ AND THE MOTION WAS INVISIBLE UNTIL 08-14. The slide and the fade in
        // shared one 0.24 s clock, so the card went solid roughly where it stops.
        // The timing lives in DyeCardTiming::entrySec now, beside the fade it
        // used to be derived from, and that field carries the measurement.
        constexpr float kCardWidth  = 330.0f;
        constexpr float kCardHeight = 66.0f;   // 62, plus room for real padding
        constexpr float kCardGap    = 8.0f;
        constexpr float kTopPad     = 24.0f;   // down from the top of the screen
        // ⚠ THE FIELD ASKED FOR THIS ONE BY NAME, TWICE: "make sure that the
        // text has padding as right now it is very close to the edges", then
        // "just make sure it has decent padding" (2026-08-14, both with
        // screenshots). It was 10, then 14.
        constexpr float kPad         = 16.0f;
        constexpr float kGapToRarity = 10.0f;  // between "Dye unlocked" and the rarity
        constexpr float kGapToSwatch = 12.0f;  // between the text's right edge and the swatch

        // ⚠ A CHIP, NOT A CAP, AND IT WENT BOTH WAYS IN ONE AFTERNOON. It was
        // taken to the card's full height flush against the right end, to match
        // the diamond on Vel'dun's TrueHUD plate, and the field put it straight
        // back: "i've decided to make the swatch smaller and not occupy the
        // entire height, make the swatch size like before" (2026-08-14). The
        // card's width came back to 330 with it, since the extra 20 existed only
        // to pay for the big square.
        constexpr float kSwatch = 34.0f;

        // ⚠ A RULE ALONG THE TOP AND THE BOTTOM, off the same reference. It is
        // NOT the border that came off with the chamfer: that was a frame on all
        // four sides, and its left-hand vertical over a near-black plate is what
        // got reported as "a thin white line to the left of text". Two
        // horizontals with no verticals cannot draw that line, and they ramp with
        // the plate so they dissolve into the margin with it.
        //
        // ⚠ TWO PIXELS AT 70%, UP FROM ONE AT 55%, because one at 55% could not
        // be found on screen ("i also couldn't see the top outline line").
        constexpr ImVec4 kRuleCol{ 0.85f, 0.83f, 0.79f, 0.70f };

        // Clip-rect breathing room on the three edges that do not have to be
        // exact.
        //
        // ⚠⚠ THE TOP RULE WAS BEING EATEN BY THE CLIP, and that is the other half
        // of "i couldn't see the top outline line". The first card is drawn at
        // the content origin, so a rule on its top edge lands exactly ON the clip
        // boundary, where half a pixel of rounding either way decides whether it
        // survives. The right edge has to stay flush because that is the screen
        // edge; the top, bottom and left do not, so they get a few pixels.
        constexpr float kSlack = 4.0f;

        // How much of the card's width the plate's fade takes, measured from the
        // LEFT edge. The rest is at full strength.
        //
        // ⚠⚠ 0.25 IS A LEGIBILITY NUMBER, NOT A TASTE ONE. Rendered at the real
        // device size over a blown-out sky: the longest name in the palette
        // ("West Weald Autumnal Orange") starts 13 px from the card's left edge
        // and the second line starts at 15% of the width, so a ramp spread across
        // the whole card puts most of the text on almost nothing and the tan
        // "Dye unlocked" line disappears outright. A fade confined to the leading
        // quarter is still zero to a hundred, which is what was asked for, and it
        // is the part of the card nothing is written on.
        //
        // ⚠ A LONGER FADE NEEDS A WIDER CARD. There are 13 px of clear space left
        // of the longest name at kCardWidth 330, so anything past a quarter eats
        // into a word. Move the width first, then the knee.
        constexpr float kFadeKnee = 0.25f;

        [[nodiscard]] float Scale() { return std::max(1.0f, FUCK::GetResolutionScale()); }

        [[nodiscard]] ImVec4 Fade(ImVec4 a_col, float a_alpha) {
            a_col.w *= a_alpha;
            return a_col;
        }

        [[nodiscard]] ImVec4 Rgb(std::uint8_t a_r, std::uint8_t a_g, std::uint8_t a_b) {
            return ImVec4(a_r / 255.0f, a_g / 255.0f, a_b / 255.0f, 1.0f);
        }

        // ---- one card ------------------------------------------------------
        //
        // ⚠ THE SWATCH IS THE RAW PALETTE COLOUR, and that is the same value the
        // grid paints rather than a second opinion about it. DyeSwatchFill in
        // EditorUI is a STATE shader: hot, held, unpaintable and locked all move
        // the colour, and every one of those is false for a card, where it
        // returns its input untouched. So this is not a second painter of one
        // appearance (two-painters-of-one-appearance-drift); it is the identity
        // case of the same chain, and a card cannot be hovered or pressed.
        void DrawCard(const DyeCard& a_card, const ImVec2& a_min, float a_alpha,
                      float a_scale) {
            if (a_alpha <= 0.0f) {
                return;
            }
            const ImVec2 max(a_min.x + kCardWidth * a_scale,
                             a_min.y + kCardHeight * a_scale);

            // ⚠⚠ A PLAIN RECTANGLE, AND THE CHAMFER IS NOT COMING BACK WITHOUT
            // A DECISION. It was cut on the left corners for one build, to match
            // the Vel'dun loot history the field pointed at, and the field's
            // verdict on seeing it was "the design of the card is cheap ... we
            // can keep it a rectangle just make sure it has decent padding"
            // (2026-08-14, with a screenshot). A rectangle with room in it beats
            // a shape with none.
            //
            // ⚠ AND THE BORDER WENT WITH IT. A one pixel tan frame at 55% over a
            // near-black plate reads as "a thin white line to the left of text",
            // which is how it was reported. The plate is 90% opaque over the
            // scene, so it has all the edge definition it needs without one.
            //
            // ⚠ THE LEFT EDGE DISSOLVES INSTEAD OF ENDING, which is Nithog's note
            // on the card ("go with gradient from zero to 100") and is how
            // Vel'dun's own loot plate reads. The right edge IS the screen edge,
            // so the plate emerges from the margin rather than sitting on it. See
            // kFadeKnee for why the fade stops a quarter of the way in, and
            // FadeRectLeft for why a gradient here is a run of bands.
            OS::ui::FadeRectLeft(a_min, max,
                                 Fade(ImVec4(0.07f, 0.07f, 0.08f, 0.90f), a_alpha),
                                 kFadeKnee);

            const bool   isMore = a_card.kind == DyeCardKind::kMore;
            const ImVec4 accent = isMore ? ImVec4(0.55f, 0.47f, 0.28f, 1.0f)
                                         : Rgb(a_card.r, a_card.g, a_card.b);

            // ⚠ THE ACCENT BAR DOWN THE LEFT EDGE IS GONE, and it is not an
            // oversight. It was the colour cue that made a stack of five
            // readable without reading a word, and the swatch moving to the
            // right (user 2026-08-14: "make the order string then to right right
            // of it the dye swatch preview") does that job from the other side.
            // A bar on the left would also have to be mitred to follow the cut,
            // and a bar on the right would sit against the swatch saying the
            // same colour twice.

            // ⚠ EVERY LINE ENDS ON ONE X, a fixed gap left of the swatch, which
            // is the second of Nithog's notes ("make text right aligned", "next
            // to square"). The swatch stays a SQUARE: the reference's diamond is
            // the half of it this card has already been told not to copy, and the
            // palette grid draws squares.
            //
            // ⚠ THE TAIL TAKES THE SAME X THOUGH IT HAS NO SWATCH, so a haul ends
            // in one column rather than one line indented by a swatch width.
            const float textRight = max.x - (kPad + kSwatch + kGapToSwatch) * a_scale;

            if (!isMore) {
                const ImVec2 swMax(max.x - kPad * a_scale,
                                   a_min.y + (kCardHeight + kSwatch) * 0.5f * a_scale);
                const ImVec2 swMin(swMax.x - kSwatch * a_scale, swMax.y - kSwatch * a_scale);
                OS::ui::RectFilled(swMin, swMax, OS::ui::Col(Fade(accent, a_alpha)));
                // ⚠ A SPECIAL DYE SHOWS BOTH STOPS, exactly as the grid does. A
                // flat chip cannot tell a pearlescent from its first stop, and a
                // card naming a colour whose swatch does not match it is worse
                // than one that never fired.
                if (a_card.secondSet) {
                    FUCK::DrawTriangleFilled(
                        ImVec2(swMax.x, swMin.y), swMax, ImVec2(swMin.x, swMax.y),
                        Fade(Rgb(a_card.r2, a_card.g2, a_card.b2), a_alpha));
                }
                FUCK::DrawRect(swMin, swMax,
                               Fade(ImVec4(0.0f, 0.0f, 0.0f, 0.65f), a_alpha), 0.0f,
                               std::max(1.0f, a_scale));
            }

            // A whole-number stroke. A rule of 1.33 px straddles two pixel rows
            // at a third coverage each, and an unantialiased fill has no way to
            // render that evenly, so a hairline drawn at the scale factor comes
            // out patchy along its length.
            const float stroke = std::max(2.0f, std::floor(a_scale * 1.5f));
            OS::ui::FadeRectLeft(a_min, ImVec2(max.x, a_min.y + stroke),
                                 Fade(kRuleCol, a_alpha), kFadeKnee);
            OS::ui::FadeRectLeft(ImVec2(a_min.x, max.y - stroke), max,
                                 Fade(kRuleCol, a_alpha), kFadeKnee);

            const float line = OS::ui::FontSize();

            if (isMore) {
                const std::string more =
                    OS::ui::FormatF("$FR_DyeCard_More"_T, static_cast<int>(a_card.more));
                // ⚠ THE SAME textRight AS A COLOUR CARD, though the tail carries
                // no swatch to sit beside. A stack ending in a tail is one column
                // of text with one ragged left edge; aligning this to its own
                // padding instead would put the last line of a haul a swatch's
                // width out from every line above it.
                OS::ui::TextAt(ImVec2(textRight - OS::ui::CalcTextWidth(more.c_str()),
                                      a_min.y + (kCardHeight * a_scale - line) * 0.5f),
                               OS::ui::Col(Fade(ImVec4(0.86f, 0.84f, 0.78f, 1.0f), a_alpha)),
                               more.c_str());
                return;
            }

            // Two lines, centred as a block. ⚠ Measured from the REAL line
            // height rather than the card's, so the pair stays centred when the
            // UI-size slider changes the font under it, and so the top line
            // cannot end up against the top edge - which is what the field saw.
            const float block = line * 2.0f + line * 0.25f;
            const float topY  = a_min.y + (kCardHeight * a_scale - block) * 0.5f;

            // ⚠ A NAME LONGER THAN THE CARD IS CUT AT THE LEFT, by the window's
            // clip rect, which lands on the card's own left edge. The longest
            // name in the shipped palette clears it by 13 px.
            OS::ui::TextAt(ImVec2(textRight - OS::ui::CalcTextWidth(a_card.name.c_str()), topY),
                           OS::ui::Col(Fade(ImVec4(0.95f, 0.94f, 0.90f, 1.0f), a_alpha)),
                           a_card.name.c_str());

            // The second line is "Dye unlocked" and the rarity, MEASURED AS ONE
            // and placed from the right as one. The rarity is left off entirely
            // when the pack did not give the colour one, which is the supported
            // way a pack says "free", and the line still lands because the pair
            // is then just the constant string.
            //
            // ⚠⚠ RIGHT-ALIGNING THE RARITY ALONE IS WHAT THE FIELD REJECTED ("i
            // do not like the alignment of the rarity next to the swatch
            // preview", 2026-08-14), and this is not that. That layout put a
            // word whose LENGTH VARIES at the right of a block whose other lines
            // started at a constant left: "Common" and "Rare" began at different
            // x, so a stack of three had three indents lining up with nothing.
            // Aligning the whole block right gives every line a constant right
            // edge by construction, and the pair moves as a unit rather than the
            // rarity sliding along a line that stands still.
            const char* const unlocked  = "$FR_DyeCard_Unlocked"_T;
            const float       subY      = topY + line * 1.25f;
            const float       unlockedW = OS::ui::CalcTextWidth(unlocked);
            const float       pairW =
                unlockedW + (a_card.rarity.empty()
                                 ? 0.0f
                                 : kGapToRarity * a_scale +
                                       OS::ui::CalcTextWidth(a_card.rarity.c_str()));
            const float pairX = textRight - pairW;
            OS::ui::TextAt(ImVec2(pairX, subY),
                           OS::ui::Col(Fade(ImVec4(0.70f, 0.62f, 0.42f, 1.0f), a_alpha)),
                           unlocked);
            if (!a_card.rarity.empty()) {
                OS::ui::TextAt(ImVec2(pairX + unlockedW + kGapToRarity * a_scale, subY),
                               OS::ui::Col(Fade(ImVec4(0.62f, 0.60f, 0.56f, 1.0f), a_alpha)),
                               a_card.rarity.c_str());
            }
        }

        // ---- the window ----------------------------------------------------
        class CardWindow final : public FUCK::IWindow {
        public:
            const char* Id() const override { return "DyeUnlockCards"; }
            const char* Title() const override { return "Fitting Room Dye Unlocks"; }

            // ⚠ A DATA QUERY, NEVER USER STATE. FUCK gates Draw on this, so the
            // stack draws exactly while it has something to say and vanishes on
            // its own. Nothing calls SetOpen and nothing may.
            bool IsOpen() const override {
                if (!g_enabled.load(std::memory_order_relaxed)) {
                    return false;
                }
                // ⚠ SUPPRESSED OVER OUR OWN EDITOR, and kCloseOnGameMenu cannot
                // do it: the editor is a FUCK IWindow, not a native game menu,
                // so FUCK's whitelist never sees it. The gold fold is already
                // saying the same thing three feet away in the dye pane (user's
                // call, 2026-08-14).
                if (OS::EditorWindow::IsOpen()) {
                    return false;
                }
                std::scoped_lock lk(g_lock);
                return !g_queue.Empty();
            }
            void SetOpen(bool) override {}

            FUCK::WindowFlags GetFlags() const override {
                using F = FUCK::WindowFlags;
                // kPassInputToGame: this is a decoration and must never become
                // an input surface. kNoBackground: each card paints its own
                // plate, so the window itself is a transparent region.
                // kRenderDuringTM: a player who has hidden the HUD with `tm` to
                // take a screenshot has not asked us to stop earning colours,
                // but they have asked for a clean frame, so this is the one flag
                // worth arguing about; it is set for consistency with every
                // other FR surface and can come off if the field disagrees.
                // kNoMove|kNoResize: the documented way to pin geometry to
                // GetDefaultPos/GetDefaultSize every frame, because an in-Draw
                // SetWindowPos lands on the host's inner ##Content child and
                // does nothing (fuck-close-on-game-menu-hides).
                //
                // ⚠⚠ kCustomPosition IS NOT ON THIS LIST AND MUST NOT GO BACK ON
                // IT. Its doc line reads "opts out of Host-managed pos
                // saving/loading", which sounds like the flag for a window that
                // computes its own position, and it is the opposite: with it set
                // the host stops applying GetDefaultPos at all and ImGui's own
                // default placement takes over. Measured in the field, one build,
                // one screenshot: the stack drew at content x 60, which is
                // ImGui's default for a new window, and the user's report was
                // "IT'S all the way off to the left side of the screen now".
                return static_cast<F>(static_cast<unsigned>(F::kNoDecoration) |
                                      static_cast<unsigned>(F::kNoBackground) |
                                      static_cast<unsigned>(F::kNoMove) |
                                      static_cast<unsigned>(F::kNoResize) |
                                      static_cast<unsigned>(F::kPassInputToGame) |
                                      static_cast<unsigned>(F::kRenderDuringTM) |
                                      static_cast<unsigned>(F::kCloseOnGameMenu));
            }

            // ⚠⚠ THE HOST INSETS CONTENT BY NOTHING, MEASURED, AND THIS FILE USED
            // TO SAY THE OPPOSITE. The comment here claimed ImGui clips to the
            // window's inner rect and that the window therefore had to carry a
            // WindowPadding on top of the card at each side. One log line ended
            // that: `content x 60.0..548.0` against a requested width of
            // 350*1.3333 + 2*pad = 488, so the content region is the WHOLE window
            // width and its left edge is the window's left edge. Nothing is inset
            // by anything.
            //
            // That padding was the gap. Subtracting it in the position put the
            // window 10.7 px too far left, which is exactly what came back as
            // "there is a gap to the right, it doesn't come out of the edge".
            //
            // ⚠ THE RIGHT EDGE IS THE ONLY ONE THAT HAS TO BE EXACT, because it
            // is the screen edge and the clip rect there is the animation: the
            // card slides in from a full width past it and that boundary is what
            // reveals it. The other three carry kSlack so a rule drawn on a
            // card's own edge cannot be eaten by the clip.
            ImVec2 GetDefaultSize() const override {
                const float s    = Scale();
                std::size_t rows = 5;
                {
                    std::scoped_lock lk(g_lock);
                    rows = std::max<std::size_t>(1, g_queue.Timing().maxVisible);
                }
                return ImVec2(kCardWidth * s + kSlack,
                              (kCardHeight + kCardGap) * static_cast<float>(rows) * s +
                                  kSlack * 2.0f);
            }

            ImVec2 GetDefaultPos() const override {
                const ImVec2 d = FUCK::GetDisplaySize();
                const float  s = Scale();
                // ⚠ THIS IS THE REQUEST, NOT THE ANSWER. Draw measures what the
                // host actually gave and aligns the card to that, so a host that
                // shifts or shrinks this window costs a few pixels of margin
                // rather than a clipped swatch.
                return ImVec2(std::max(0.0f, d.x - kCardWidth * s - kSlack),
                              kTopPad * s - kSlack);
            }

            void Draw() override {
                const double now = FUCK::GetTime();
                const float  s   = Scale();

                std::vector<DyeCard> snapshot;
                std::vector<float>   alphas;
                std::vector<float>   slides;
                bool                 woke = false;
                {
                    std::scoped_lock lk(g_lock);
                    // ⚠ THE CLOCK RUNS HERE AND ONLY HERE. IsOpen is const and a
                    // query; giving it the side effect would tick the queue from
                    // whatever cadence the host polls it at.
                    g_queue.Tick(now);
                    woke     = g_queue.WokeFromEmpty();
                    snapshot = g_queue.Visible();
                    alphas.reserve(snapshot.size());
                    slides.reserve(snapshot.size());
                    for (const auto& c : snapshot) {
                        alphas.push_back(g_queue.AlphaOf(c, now));
                        slides.push_back(g_queue.SlideOf(c, now));
                    }
                }

                // ---- the cue -----------------------------------------------
                //
                // ⚠ ONCE PER HAUL, ON THE EDGE THE QUEUE REPORTS, not once per
                // card. Cards enter a third of a second apart and a haul can be
                // six of them, so a cue per card is a machine gun; the queue
                // says when the stack went from empty to occupied and that is
                // the moment a player notices something arrived.
                //
                // ⚠ HERE RATHER THAN IN Announce, because Announce can fire
                // while the editor is open, where IsOpen suppresses the whole
                // stack. A chime with no card is worse than a card with no
                // chime. Drawing is the proof the player is looking at it.
                //
                // ⚠ Played straight from the present thread, which is what every
                // other cue in this editor does (EditorStyle::PlayUISound has
                // ~105 call sites, all of them in draw code).
                if (woke) {
                    const auto& snd = OS::Settings::GetSingleton().dyeCardSound;
                    if (!snd.empty()) {
                        EditorStyle::PlayUISound(snd.c_str());
                    }
                }

                // ---- where the right edge actually is ----------------------
                //
                // ⚠⚠ MEASURED EVERY FRAME, NEVER ASSUMED, because assuming it is
                // what the field saw: "there is a gap to the right, it doesn't
                // come out of the edge". The old code took GetCursorScreenPos and
                // added a card width, which is only the screen edge while the
                // window sits exactly where GetDefaultPos asked. The content rect
                // is the thing that is true whatever the host did with the
                // request, and GetContentRegionAvail is how to read its far side.
                //
                // ⚠ AND IT IS CLAMPED TO THE DISPLAY, which is the other half. If
                // the window is allowed past the screen edge, the content rect
                // runs past it too, and a card aligned to that would hang its
                // swatch off the screen. min() takes whichever edge comes first,
                // so the plate lands on the screen edge when it can reach it and
                // on the content edge when it cannot.
                const ImVec2 origin = FUCK::GetCursorScreenPos();
                const ImVec2 disp   = FUCK::GetDisplaySize();
                const float  right  =
                    std::min(origin.x + FUCK::GetContentRegionAvail().x, disp.x);
                const float  left   = right - kCardWidth * s;

                // ⚠ ONE LINE PER HAUL, NOT PER FRAME, and it earns its place: the
                // gap above is invisible in a screenshot until someone measures
                // the pixels, and this says the number outright. Drop it once the
                // field confirms the plate reaches the edge.
                if (woke) {
                    spdlog::info("Dye unlock cards: content x {:.1f}..{:.1f}, display {:.1f}, "
                                 "card {:.1f}..{:.1f}, so the plate {} the screen edge.",
                                 origin.x, origin.x + FUCK::GetContentRegionAvail().x,
                                 disp.x, left, right,
                                 (disp.x - right) < 0.5f ? "REACHES" : "STOPS SHORT OF");
                }

                for (std::size_t i = 0; i < snapshot.size(); ++i) {
                    // A whole card width, so it starts entirely past the screen
                    // edge and the window's clip rect is what reveals it.
                    //
                    // ⚠ kSlack DOWN FROM THE CONTENT TOP, so the first card's top
                    // rule is inside the clip rather than on its boundary.
                    const ImVec2 at(left + slides[i] * kCardWidth * s,
                                    origin.y + kSlack +
                                        static_cast<float>(i) * (kCardHeight + kCardGap) * s);
                    DrawCard(snapshot[i], at, alphas[i], s);
                }
            }
        };

        CardWindow g_window;

    }  // namespace

    void Register() {
        ApplySettings();
        FUCK::RegisterWindow(&g_window);
        spdlog::info("Dye unlock cards: registered as a FUCK IWindow, so an earned "
                     "colour can be announced during gameplay.");
    }

    void ApplySettings() {
        const auto&   cfg = OS::Settings::GetSingleton();
        DyeCardTiming t;
        t.maxVisible = static_cast<std::size_t>(std::clamp(cfg.dyeCardMax, 1, 8));
        t.dwellSec   = std::clamp(cfg.dyeCardDwell, 2.0f, 20.0f);
        t.burstCap   = static_cast<std::size_t>(std::clamp(cfg.dyeCardBurst, 2, 12));
        {
            std::scoped_lock lk(g_lock);
            g_queue.SetTiming(t);
        }
        // ⚠ AND THE ECONOMY SWITCH (2026-09-04). With bDyeUnlocks off every
        // colour is already pickable, so "Dye unlocked" announces nothing; the
        // field had the switch off in every session read and the cards still
        // fired, because only the pane's padlock ever read it.
        const bool on = OS::AnnouncesCards(cfg.dyeUnlocks, cfg.dyeUnlockCards);
        g_enabled.store(on, std::memory_order_relaxed);
        if (!on) {
            // Switched off mid-session: what is already on screen goes too,
            // rather than finishing its dwell after the player said stop.
            std::scoped_lock lk(g_lock);
            g_queue.Clear();
        }
    }

    void Preview() {
        // Real colours, spread across the palette rather than the first few, so
        // the sample shows a range of hues and stands a decent chance of
        // including a two-stop dye.
        const auto palette = OS::DyePalette::Snapshot();
        if (palette.empty()) {
            spdlog::warn("Dye unlock cards: preview asked for, but the palette is "
                         "empty. Check that a Dyes directory is reachable.");
            return;
        }
        std::size_t want = 3;
        {
            std::scoped_lock lk(g_lock);
            want = std::min<std::size_t>(g_queue.Timing().maxVisible, 3);
        }
        want = std::max<std::size_t>(1, std::min(want, palette.size()));

        std::vector<DyeCard> cards;
        cards.reserve(want);
        const std::size_t step = std::max<std::size_t>(1, palette.size() / want);
        for (std::size_t i = 0; i < want; ++i) {
            const auto& dye = palette[std::min(i * step, palette.size() - 1)];
            DyeCard     c;
            c.kind      = DyeCardKind::kColour;
            c.id        = dye.id;
            c.name      = dye.name;
            c.rarity    = dye.rarity;
            c.r         = dye.colour.r;
            c.g         = dye.colour.g;
            c.b         = dye.colour.b;
            c.secondSet = dye.colour.secondSet;
            c.r2        = dye.colour.r2;
            c.g2        = dye.colour.g2;
            c.b2        = dye.colour.b2;
            cards.push_back(std::move(c));
        }
        {
            std::scoped_lock lk(g_lock);
            g_queue.Push(std::move(cards), 0.0);
        }
        // ⚠ NOT GATED ON g_enabled EITHER. Pressing the button with the setting
        // off and seeing nothing happen is indistinguishable from the feature
        // being broken, which is the confusion this whole function exists to
        // end. The window's own IsOpen still respects the setting, so the honest
        // fix is to say so rather than to draw anyway.
        if (!g_enabled.load(std::memory_order_relaxed)) {
            spdlog::info("Dye unlock cards: preview queued, but the cards are "
                         "switched OFF, so nothing will draw. Tick the setting "
                         "beside the button.");
            return;
        }
        spdlog::info("Dye unlock cards: preview queued with {} sample colour(s).",
                     want);
    }

    void Arm() { g_armed.store(true, std::memory_order_release); }
    bool Armed() { return g_armed.load(std::memory_order_acquire); }

    void Forget() {
        g_armed.store(false, std::memory_order_release);
        std::scoped_lock lk(g_lock);
        g_queue.Clear();
    }

    void Announce(const std::vector<std::string>& a_ids) {
        if (a_ids.empty()) {
            return;
        }
        // ⚠ THE BASELINE. Everything the load-time passes earn is the save
        // catching up with rules it has never been graded against, not something
        // that happened while the player was watching. It is absorbed in silence
        // and the gold fold still marks every one of it in the grid.
        if (!g_armed.load(std::memory_order_acquire)) {
            spdlog::info("Dye unlock cards: {} colour(s) absorbed as the baseline, "
                         "because the announcer is not armed yet. The gold fold "
                         "still marks them in the grid.",
                         a_ids.size());
            return;
        }
        if (!g_enabled.load(std::memory_order_relaxed)) {
            return;
        }

        std::vector<DyeCard> cards;
        cards.reserve(a_ids.size());
        for (const auto& id : a_ids) {
            const auto dye = OS::DyePalette::Find(id);
            if (!dye) {
                // ⚠ A GAINED ID WITH NO PALETTE ENTRY IS NOT DRAWABLE and it is
                // not worth inventing a name for. Promotion only ever grants ids
                // it read OFF the palette, so this is unreachable unless a pack
                // is unloaded between the two, and a silent skip is the right
                // failure: the colour is still earned and still in the grid.
                spdlog::warn("Dye unlock cards: earned id \"{}\" is not in the "
                             "palette, so it gets no card. It is still earned.",
                             id);
                continue;
            }
            DyeCard c;
            c.kind      = DyeCardKind::kColour;
            c.id        = id;
            c.name      = dye->name;
            c.rarity    = dye->rarity;
            c.r         = dye->colour.r;
            c.g         = dye->colour.g;
            c.b         = dye->colour.b;
            c.secondSet = dye->colour.secondSet;
            c.r2        = dye->colour.r2;
            c.g2        = dye->colour.g2;
            c.b2        = dye->colour.b2;
            cards.push_back(std::move(c));
        }
        if (cards.empty()) {
            return;
        }
        const std::size_t n = cards.size();
        {
            std::scoped_lock lk(g_lock);
            g_queue.Push(std::move(cards), 0.0);
        }
        spdlog::info("Dye unlock cards: announcing {} newly earned colour(s).", n);
    }

}  // namespace OS::DyeUnlockCard
