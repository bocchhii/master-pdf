#include "view/SearchResults.h"

#include "ui/ScrollBox.h"
#include "ui/Theme.h"
#include "ui/ToolButton.h"

#include <QHBoxLayout>
#include <QLabel>
#include <QScrollBar>
#include <QTextBlock>
#include <QTextBrowser>
#include <QVBoxLayout>

SearchResults::SearchResults(QWidget *parent) : QWidget(parent)
{
    auto *box = new QVBoxLayout(this);
    box->setContentsMargins(0, 0, 0, 0);
    box->setSpacing(2);
    auto *head = new QHBoxLayout;
    head->setContentsMargins(2, 0, 2, 0);
    head->setSpacing(0);
    count_ = new QLabel(this);
    head->addWidget(count_, 1);
    // (packed from the right, like the Python app's: Next at the end, Previous before it)
    auto *up = new ToolButton(QStringLiteral("up"), tr("Previous result (Shift + Enter)"), {}, false, {}, this);
    auto *down = new ToolButton(QStringLiteral("down"), tr("Next result (Enter)"), {}, false, {}, this);
    up->setWhiten(16);
    down->setWhiten(16);
    connect(up, &ToolButton::clicked, this, [this] { emit step(-1); });
    connect(down, &ToolButton::clicked, this, [this] { emit step(1); });
    head->addWidget(up);
    head->addWidget(down);
    box->addLayout(head);
    list_ = new QTextBrowser(this);
    list_->setOpenLinks(false);
    list_->viewport()->setCursor(Qt::ArrowCursor);
    list_->setTextInteractionFlags(Qt::LinksAccessibleByMouse);
    list_->document()->setDocumentMargin(3);
    connect(list_, &QTextBrowser::anchorClicked, this,
            [this](const QUrl &url) { emit resultClicked(url.toString().toInt()); });
    box->addWidget(new ScrollBox(list_, false, this), 1);
}

void SearchResults::fill(const std::vector<FindHit> &hits, const QString &text)
{
    const QString ink = theme::text().name();
    QString html = QStringLiteral(
        "<style>a{color:%1;text-decoration:none} .where{color:%2}"
        " p{margin-top:2px;margin-bottom:4px}</style>")
                       .arg(ink, theme::c("#808080", theme::Text).name());
    if (hits.empty() && !text.isEmpty())
        html += QStringLiteral("<p align=center style='color:%1'><br>%2</p>")
                    .arg(theme::c("#808080", theme::Text).name(),
                         tr("No results for \"%1\".").arg(text.toHtmlEscaped()));
    for (size_t k = 0; k < hits.size(); ++k) {
        const FindHit &h = hits[k];
        html += QStringLiteral("<p><a href=\"%1\">%2<b>%3</b>%4<span class=where>&nbsp;&nbsp; p.&nbsp;%5</span></a></p>")
                    .arg(k)
                    .arg(h.before.toHtmlEscaped(), h.words.toHtmlEscaped(), h.after.toHtmlEscaped())
                    .arg(h.page + 1);
    }
    list_->setHtml(html);
    list_->setExtraSelections({});
    if (hits.empty())
        count_->setText(text.isEmpty() ? QString() : tr("No results"));
    else
        count_->setText(hits.size() == 1 ? tr("1 result") : tr("%1 results").arg(hits.size()));
}

void SearchResults::setResult(int k, int total)
{
    count_->setText(tr("Result %1 of %2").arg(k + 1).arg(total));
    const QTextBlock block = list_->document()->findBlockByNumber(k);
    if (!block.isValid())
        return;
    QTextEdit::ExtraSelection sel;
    sel.cursor = QTextCursor(block);
    const auto [bg, fg] = theme::selection();
    sel.format.setBackground(bg);
    sel.format.setForeground(fg);
    sel.cursor.movePosition(QTextCursor::EndOfBlock, QTextCursor::KeepAnchor);
    list_->setExtraSelections({sel});
    QTextCursor at(block);
    list_->setTextCursor(at);
    list_->ensureCursorVisible();
    at.movePosition(QTextCursor::EndOfBlock);
    list_->setTextCursor(at);
    list_->ensureCursorVisible();
}
