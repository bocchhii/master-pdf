#pragma once

#include <QStringList>
#include <QWidget>

// The tabs of the PDFs open in the window - Home first - drawn as Windows 98's property sheet tabs:
// raised, the one chosen a little taller and joined to what's under it. A PDF's tab has a little x
// to close it (a middle click closes it too); Home has none.
class DocTabs : public QWidget {
    Q_OBJECT
public:
    explicit DocTabs(QWidget *parent = nullptr);

    int addTab(const QString &text, const QString &tip, bool closable);
    void removeTab(int index);
    void setTabText(int index, const QString &text, const QString &tip);
    int count() const { return int(tabs_.size()); }
    int current() const { return current_; }
    void setCurrent(int index);  // (emits currentChanged when it changes)
    QRect tabRect(int index) const;   // (for the self-test: where a tab is)
    QRect closeRect(int index) const; // ... and its x
    QSize sizeHint() const override;
    QSize minimumSizeHint() const override;

signals:
    void currentChanged(int index);
    void closeRequested(int index);

protected:
    void paintEvent(QPaintEvent *) override;
    void mousePressEvent(QMouseEvent *) override;
    void mouseReleaseEvent(QMouseEvent *) override;
    void mouseMoveEvent(QMouseEvent *) override;
    void leaveEvent(QEvent *) override;
    void resizeEvent(QResizeEvent *) override;
    bool event(QEvent *) override;

private:
    struct Tab {
        QString text, tip;
        bool closable = false;
        int width = 0;  // (laid out)
    };
    void layoutTabs();
    int tabAt(QPoint pos) const;
    std::vector<Tab> tabs_;
    int current_ = -1;
    int pressedClose_ = -1;  // (its x held down)
    int hoverClose_ = -1;
};
