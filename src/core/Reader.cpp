// The text reader (step 7): that part of the page drawn big and clear, read by Windows' text
// reader (Windows.Media.Ocr, run by PowerShell) in Arabic and in English; Arabic used if it
// found Arabic letters, English otherwise.

#include "core/Reader.h"

#include "core/Document.h"

#include <QCoreApplication>
#include <QDir>
#include <QPainter>
#include <QProcess>
#include <QRegularExpression>
#include <QTemporaryDir>

#include <algorithm>
#include <cmath>

namespace reader {

namespace {

const char *script = R"(
$ErrorActionPreference = 'Stop'
[Console]::OutputEncoding = [Text.Encoding]::UTF8
Add-Type -AssemblyName System.Runtime.WindowsRuntime
$null = [Windows.Storage.StorageFile, Windows.Storage, ContentType = WindowsRuntime]
$null = [Windows.Media.Ocr.OcrEngine, Windows.Foundation, ContentType = WindowsRuntime]
$null = [Windows.Graphics.Imaging.BitmapDecoder, Windows.Foundation, ContentType = WindowsRuntime]
$null = [Windows.Globalization.Language, Windows.Foundation, ContentType = WindowsRuntime]
$asTask = ([System.WindowsRuntimeSystemExtensions].GetMethods() | Where-Object {
    $_.Name -eq 'AsTask' -and $_.GetParameters().Count -eq 1 -and $_.GetParameters()[0].ParameterType.Name -eq 'IAsyncOperation`1' })[0]
function Await($op, $type) {
    $task = $asTask.MakeGenericMethod($type).Invoke($null, @($op))
    [void]$task.Wait(-1)
    $task.Result
}
$file = Await ([Windows.Storage.StorageFile]::GetFileFromPathAsync($env:MP_PNG)) ([Windows.Storage.StorageFile])
$stream = Await ($file.OpenAsync([Windows.Storage.FileAccessMode]::Read)) ([Windows.Storage.Streams.IRandomAccessStream])
$decoder = Await ([Windows.Graphics.Imaging.BitmapDecoder]::CreateAsync($stream)) ([Windows.Graphics.Imaging.BitmapDecoder])
$bitmap = Await ($decoder.GetSoftwareBitmapAsync()) ([Windows.Graphics.Imaging.SoftwareBitmap])
foreach ($tag in 'ar-SA', 'en-US') {
    $lang = [Windows.Globalization.Language]::new($tag)
    if (-not [Windows.Media.Ocr.OcrEngine]::IsLanguageSupported($lang)) { continue }
    $engine = [Windows.Media.Ocr.OcrEngine]::TryCreateFromLanguage($lang)
    $result = Await ($engine.RecognizeAsync($bitmap)) ([Windows.Media.Ocr.OcrResult])
    "#$tag"
    foreach ($line in $result.Lines) {
        $x0 = 1e9; $y0 = 1e9; $x1 = -1e9; $y1 = -1e9
        foreach ($w in $line.Words) {
            $r = $w.BoundingRect
            $x0 = [Math]::Min($x0, $r.X); $y0 = [Math]::Min($y0, $r.Y)
            $x1 = [Math]::Max($x1, $r.X + $r.Width); $y1 = [Math]::Max($y1, $r.Y + $r.Height)
        }
        '{0} {1} {2} {3}' -f [int]$x0, [int]$y0, [int]$x1, [int]$y1
        $line.Text
    }
}
)";

using Lines = std::vector<std::pair<QString, QRectF>>;

}  // namespace

Lines read(Document &doc, int page, const QRectF &rect, QString *error)
{
    // drawn about 48 px a line high (the reader's best), never past its 2600 px limit, with
    // white round it
    const QRectF area = rect.adjusted(-rect.height() * 0.3, -rect.height() * 0.3, rect.height() * 0.3, rect.height() * 0.3);
    double scale = std::clamp(48.0 / std::max(1.0, std::min(rect.height(), 40.0)), 1.0, 8.0);
    scale = std::min(scale, 2500.0 / std::max(area.width(), area.height()));
    const QRect px = QRectF(area.topLeft() * scale, area.size() * scale).toAlignedRect();
    QImage drawn = doc.render(page, scale, px);
    if (drawn.isNull()) {
        if (error)
            *error = QCoreApplication::translate("reader", "That part of the page couldn't be drawn.");
        return {};
    }
    QImage image(drawn.size(), QImage::Format_RGB32);
    image.fill(Qt::white);
    QPainter(&image).drawImage(0, 0, drawn);
    QTemporaryDir dir;
    const QString png = dir.filePath(QStringLiteral("read.png"));
    if (!dir.isValid() || !image.save(png)) {
        if (error)
            *error = QCoreApplication::translate("reader", "The picture to read couldn't be saved.");
        return {};
    }
    QProcess ps;
    QProcessEnvironment env = QProcessEnvironment::systemEnvironment();
    env.insert(QStringLiteral("MP_PNG"), QDir::toNativeSeparators(png));
    ps.setProcessEnvironment(env);
    ps.start(QStringLiteral("powershell.exe"), {QStringLiteral("-NoProfile"), QStringLiteral("-ExecutionPolicy"), QStringLiteral("Bypass"),
                                                QStringLiteral("-Command"), QString::fromLatin1(script)});
    if (!ps.waitForFinished(60000)) {
        ps.kill();
        if (error)
            *error = QCoreApplication::translate("reader", "Windows' text reader didn't answer.");
        return {};
    }
    const QStringList out = QString::fromUtf8(ps.readAllStandardOutput()).split(QLatin1Char('\n'));
    Lines arabic, english, *now = nullptr;
    for (int i = 0; i < out.size(); ++i) {
        const QString line = out[i].trimmed();
        if (line == QLatin1String("#ar-SA")) {
            now = &arabic;
        } else if (line == QLatin1String("#en-US")) {
            now = &english;
        } else if (now && i + 1 < out.size()) {
            const QStringList n = line.split(QLatin1Char(' '));
            if (n.size() != 4)
                continue;
            const QString text = out[++i].trimmed();
            if (text.isEmpty())
                continue;
            const QRectF r(QPointF(n[0].toDouble() / scale + area.left(), n[1].toDouble() / scale + area.top()),
                           QPointF(n[2].toDouble() / scale + area.left(), n[3].toDouble() / scale + area.top()));
            now->emplace_back(text, r);
        }
    }
    static const QRegularExpression arabicLetters(QStringLiteral("[\\x{0600}-\\x{06FF}]"));
    bool hasArabic = false;
    for (const auto &l : arabic)
        hasArabic = hasArabic || l.first.contains(arabicLetters);
    Lines lines = hasArabic ? arabic : english;
    if (hasArabic)  // the reader gives an Arabic line's words left to right, as seen: put in
        for (auto &l : lines) {  // reading order (runs of English words, numbers kept as they are)
            const QStringList seen = l.first.simplified().split(QLatin1Char(' '));
            QStringList words;
            for (int i = seen.size() - 1; i >= 0;) {
                if (seen[i].contains(arabicLetters)) {
                    words << seen[i--];
                    continue;
                }
                int j = i;
                while (j > 0 && !seen[j - 1].contains(arabicLetters))
                    --j;
                words << seen.mid(j, i - j + 1);
                i = j - 1;
            }
            l.first = words.join(QLatin1Char(' '));
        }
    if (lines.empty() && error)
        *error = ps.exitCode() != 0 ? QCoreApplication::translate("reader", "Windows' text reader isn't available here.")
                                    : QCoreApplication::translate("reader", "No words were found there.");
    return lines;
}

}  // namespace reader
