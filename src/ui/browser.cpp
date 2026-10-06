#include "ui/browser.hpp"

#include <stdio.h>

#include "ui/font.hpp"
#include "ui/theme.hpp"

namespace {
constexpr int kRowH = 24;
constexpr int kListY = kViewTop + 54;
constexpr int kVisibleRows = 10;
} // namespace

const char* BrowserView::hint() const
{
    return G_CROSS " ASSIGN TO CHANNEL  " G_SQUARE " PREVIEW SW  " G_TRIANGLE " PREVIEW SPU2  L1/R1 CHANNEL";
}

void BrowserView::update(const InputState& in, UiContext& ctx)
{
    const int n = ctx.bank.count();
    if (n == 0)
        return;
    if (in.rep(btn::Up))
        sel_ = sel_ > 0 ? sel_ - 1 : 0;
    if (in.rep(btn::Down))
        sel_ = sel_ < n - 1 ? sel_ + 1 : n - 1;
    if (sel_ < first_)
        first_ = sel_;
    if (sel_ >= first_ + kVisibleRows)
        first_ = sel_ - kVisibleRows + 1;

    const int channels = ctx.session.project().channelCount;
    if (in.rep(btn::L1))
        ctx.selectedChannel = (ctx.selectedChannel + channels - 1) % channels;
    if (in.rep(btn::R1))
        ctx.selectedChannel = (ctx.selectedChannel + 1) % channels;

    if (in.hit(btn::Cross)) {
        ctx.session.setSample(ctx.selectedChannel, sel_);
        ctx.toast("Channel %d <- %s", ctx.selectedChannel + 1, ctx.bank.get(sel_) ? ctx.bank.get(sel_)->name : "?");
    }
    if (in.hit(btn::Square))
        ctx.session.previewSample(sel_, VoiceMode::Software);
    if (in.hit(btn::Triangle)) {
        if (ctx.audio.stats().spuSounds > 0)
            ctx.session.previewSample(sel_, VoiceMode::Spu2);
        else
            ctx.toast("No samples in SPU2 RAM (see PROJECT > status)");
    }
}

void BrowserView::draw(Gfx& g, UiContext& ctx)
{
    const int x = theme::kSafeLeft, w = theme::kSafeRight - theme::kSafeLeft;
    ui::panel(g, x, kViewTop, w, kViewBottom - kViewTop, "SAMPLE BROWSER");

    const ChannelData& c = ctx.session.project().channels[ctx.selectedChannel];
    g.textf(x + 8, kViewTop + 28, theme::kTextDim, "TARGET: CH%d %s   SOURCE: BUILT-IN KIT", ctx.selectedChannel + 1, c.name);

    const SampleBank& bank = ctx.bank;
    for (int r = 0; r < kVisibleRows; ++r) {
        const int i = first_ + r;
        if (i >= bank.count())
            break;
        const Sample* s = bank.get(i);
        if (!s)
            continue;
        const int y = kListY + r * kRowH;
        ui::listRow(g, x + 6, y, w - 30, kRowH - 2, i == sel_);
        g.textf(x + 16, y + 3, i == sel_ ? theme::kText : theme::kTextDim, "%2d  %-8s %5lu ms  %5lu Hz  %s", i + 1, s->name,
                (unsigned long)(s->frames * 1000u / s->sampleRate), (unsigned long)s->sampleRate, s->channels == 2 ? "STEREO" : "MONO");
        if (c.sampleSlot == i)
            g.text(x + w - 60, y + 3, G_NOTE, theme::kAccent);
    }
    ui::scrollbar(g, x + w - 18, kListY, 8, kVisibleRows * kRowH, first_, kVisibleRows, bank.count());

    const char* usb = ctx.storage.ready() ? ctx.storage.rootName() : "no USB drive";
    char footer[96];
    snprintf(footer, sizeof(footer), "USB: %s   WAV/ADP import from USB arrives in Milestone 2", usb);
    g.text(x + 8, kViewBottom - 22, footer, theme::kTextDim, 1, 2);
}
