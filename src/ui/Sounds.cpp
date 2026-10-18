#include "ui/Sounds.h"

#include "ui/Theme.h"

#include <QByteArray>

#include <algorithm>
#include <cmath>
#include <thread>
#include <vector>

#ifdef Q_OS_WIN
#include <windows.h>
#include <mmsystem.h>
#endif

namespace sounds {

namespace {

// WAV bytes (the Python app's make_sound): notes with quieter overtones that fade faster, a
// soft start, a little room echo, a comfortable loudness
QByteArray make(Kind kind)
{
    const int rate = 44100;
    struct Note {
        double start;
        std::vector<double> freqs;
    };
    std::vector<Note> notes;
    double decay;
    if (kind == Done) {
        notes = {{0.00, {784.0}}, {0.11, {1174.7}}};
        decay = 5.0;
    } else {
        notes = {{0.00, {329.6, 493.9}}, {0.13, {220.0, 329.6}}};
        decay = 6.0;
    }
    const int n = rate;  // (a second)
    std::vector<double> out(size_t(n), 0.0);
    for (const Note &note : notes)
        for (size_t i = 0; i < note.freqs.size(); ++i) {
            const double f = note.freqs[i], level = i == 0 ? 1.0 : 0.45;
            for (int k = 0; k < n; ++k) {
                const double t = double(k) / rate;
                if (t < note.start)
                    continue;
                const double tt = t - note.start;
                const double tone = std::sin(2 * M_PI * f * tt) + 0.35 * std::sin(2 * M_PI * 2 * f * tt) * std::exp(-tt * decay * 1.5) +
                                    0.12 * std::sin(2 * M_PI * 3 * f * tt) * std::exp(-tt * decay * 3);
                out[size_t(k)] += level * tone * std::exp(-tt * decay) * std::min(tt / 0.004, 1.0);
            }
        }
    for (const auto &[delay, gain] : {std::pair{0.045, 0.25}, std::pair{0.09, 0.14}, std::pair{0.14, 0.08}}) {
        const int d = int(rate * delay);
        for (int k = n - 1; k >= d; --k)
            out[size_t(k)] += gain * out[size_t(k - d)];
    }
    double peak = 1e-9;
    for (double v : out)
        peak = std::max(peak, std::abs(v));
    QByteArray pcm;
    pcm.resize(n * 2);
    for (int k = 0; k < n; ++k) {
        const double fade = std::min(double(n - k) / (rate * 0.05), 1.0);
        const auto s = qint16(std::lround(out[size_t(k)] * 0.45 / peak * fade * 32767));
        pcm[2 * k] = char(s & 0xFF);
        pcm[2 * k + 1] = char((s >> 8) & 0xFF);
    }
    QByteArray wav;
    auto u32 = [&](quint32 v) {
        for (int i = 0; i < 4; ++i)
            wav.append(char((v >> (8 * i)) & 0xFF));
    };
    auto u16 = [&](quint16 v) {
        wav.append(char(v & 0xFF));
        wav.append(char(v >> 8));
    };
    wav.append("RIFF");
    u32(quint32(36 + pcm.size()));
    wav.append("WAVEfmt ");
    u32(16);
    u16(1);  // (PCM)
    u16(1);  // (one channel)
    u32(rate);
    u32(rate * 2);
    u16(2);
    u16(16);
    wav.append("data");
    u32(quint32(pcm.size()));
    wav.append(pcm);
    return wav;
}

}  // namespace

void play(Kind kind)
{
    if (!theme::settings().value("sounds").toBool(true))
        return;
#ifdef Q_OS_WIN
    std::thread([kind] {
        static QByteArray done = make(Done), error = make(Error);
        const QByteArray &wav = kind == Done ? done : error;
        PlaySoundA(wav.constData(), nullptr, SND_MEMORY | SND_NODEFAULT);
    }).detach();
#else
    Q_UNUSED(kind);
#endif
}

}  // namespace sounds
