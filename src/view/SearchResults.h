#pragma once

#include "core/PageText.h"

#include <QWidget>

#include <vector>

class QLabel;
class QTextBrowser;

// Find's results, like Word's navigation pane: "Result 3 of 60" with buttons for the one
// before and after, then each place the words are - its line, the words in bold, the
// page - to click and go there.
class SearchResults : public QWidget {
    Q_OBJECT
public:
    explicit SearchResults(QWidget *parent = nullptr);

    void fill(const std::vector<FindHit> &hits, const QString &text);
    void setResult(int k, int total);  // the one you're at: marked (scrolled into sight)

signals:
    void resultClicked(int k);
    void step(int by);  // the next (1) or previous (-1) result

private:
    QLabel *count_ = nullptr;
    QTextBrowser *list_ = nullptr;
};
