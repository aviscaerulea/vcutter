// RangeSlider ユニットテスト
// 公開 setter は内部状態 getter を持たないため、シグナル発火と QSlider 継承の value() のみで検証する
// grab() でのピクセル比較は不安定かつテスト価値が低いため不採用

#include <QtTest/QtTest>
#include <QSignalSpy>
#include <QApplication>
#include <QMouseEvent>
#include <QWheelEvent>
#include <QPointF>

#include "RangeSlider.h"

namespace {

// RangeSlider が値変換に使う slider span を確定させるための統一サイズ
constexpr int kWidth  = 200;
constexpr int kHeight = RangeSlider::kTotalH;

// 指定 x 位置でマウスボタンを押下する
void pressLeft(RangeSlider* slider, int x)
{
    QPointF local(x, kHeight / 2);
    QMouseEvent ev(QEvent::MouseButtonPress, local, slider->mapToGlobal(local.toPoint()),
                   Qt::LeftButton, Qt::LeftButton, Qt::NoModifier);
    QApplication::sendEvent(slider, &ev);
}

// 指定 x 位置でマウスを移動させる（左ボタン押下中の想定）
void moveLeft(RangeSlider* slider, int x)
{
    QPointF local(x, kHeight / 2);
    QMouseEvent ev(QEvent::MouseMove, local, slider->mapToGlobal(local.toPoint()),
                   Qt::LeftButton, Qt::LeftButton, Qt::NoModifier);
    QApplication::sendEvent(slider, &ev);
}

// 指定 x 位置でマウスボタンを離す
void releaseLeft(RangeSlider* slider, int x)
{
    QPointF local(x, kHeight / 2);
    QMouseEvent ev(QEvent::MouseButtonRelease, local, slider->mapToGlobal(local.toPoint()),
                   Qt::LeftButton, Qt::NoButton, Qt::NoModifier);
    QApplication::sendEvent(slider, &ev);
}

// 指定 angleDelta と修飾キーでホイールイベントを送る
void wheel(RangeSlider* slider, int angleDeltaY, Qt::KeyboardModifiers mods = Qt::NoModifier)
{
    QPointF local(kWidth / 2, kHeight / 2);
    QPoint  pixelDelta(0, 0);
    QPoint  angleDelta(0, angleDeltaY);
    QWheelEvent ev(local, slider->mapToGlobal(local.toPoint()),
                   pixelDelta, angleDelta,
                   Qt::NoButton, mods,
                   Qt::NoScrollPhase, false);
    QApplication::sendEvent(slider, &ev);
}

} // namespace

class TestRangeSlider : public QObject
{
    Q_OBJECT

private slots:
    void init();
    void cleanup();

    // ホイール
    void wheelScrolled_forwardOnPositiveDelta();
    void wheelScrolled_backwardOnNegativeDelta();
    void wheelScrolled_zeroDeltaSuppressed();
    void wheelScrolled_carriesModifiers();

    // ホバー
    void hoverMoved_emitsXWithinBounds();
    void hoverLeft_onLeaveEvent();

    // ドラッグ
    void dragStarted_firesOnFirstMoveAfterPress();

private:
    RangeSlider* m_slider = nullptr;
};

void TestRangeSlider::init()
{
    m_slider = new RangeSlider(Qt::Horizontal);
    m_slider->setRange(0, 1000);
    m_slider->resize(kWidth, kHeight);
}

void TestRangeSlider::cleanup()
{
    delete m_slider;
    m_slider = nullptr;
}

void TestRangeSlider::wheelScrolled_forwardOnPositiveDelta()
{
    QSignalSpy spy(m_slider, &RangeSlider::wheelScrolled);
    wheel(m_slider, +120);
    QCOMPARE(spy.count(), 1);
    QCOMPARE(spy.first().at(0).toBool(), true);   // forward
    QCOMPARE(spy.first().at(1).toBool(), false);  // shift
    QCOMPARE(spy.first().at(2).toBool(), false);  // ctrl
}

void TestRangeSlider::wheelScrolled_backwardOnNegativeDelta()
{
    QSignalSpy spy(m_slider, &RangeSlider::wheelScrolled);
    wheel(m_slider, -120);
    QCOMPARE(spy.count(), 1);
    QCOMPARE(spy.first().at(0).toBool(), false);
}

void TestRangeSlider::wheelScrolled_zeroDeltaSuppressed()
{
    QSignalSpy spy(m_slider, &RangeSlider::wheelScrolled);
    wheel(m_slider, 0);
    QCOMPARE(spy.count(), 0);
}

void TestRangeSlider::wheelScrolled_carriesModifiers()
{
    QSignalSpy spy(m_slider, &RangeSlider::wheelScrolled);
    wheel(m_slider, +120, Qt::ShiftModifier | Qt::ControlModifier);
    QCOMPARE(spy.count(), 1);
    QCOMPARE(spy.first().at(1).toBool(), true);  // shift
    QCOMPARE(spy.first().at(2).toBool(), true);  // ctrl
}

void TestRangeSlider::hoverMoved_emitsXWithinBounds()
{
    QSignalSpy spy(m_slider, &RangeSlider::hoverMoved);
    // ボタン非押下でホバー。x=150 を送ると [0, width-1] 内へクランプされた値が返る
    QPointF local(150, kHeight / 2);
    QMouseEvent ev(QEvent::MouseMove, local, m_slider->mapToGlobal(local.toPoint()),
                   Qt::NoButton, Qt::NoButton, Qt::NoModifier);
    QApplication::sendEvent(m_slider, &ev);

    QVERIFY(spy.count() >= 1);
    const int x = spy.last().at(0).toInt();
    QVERIFY(x >= 0 && x < kWidth);
}

void TestRangeSlider::hoverLeft_onLeaveEvent()
{
    QSignalSpy spy(m_slider, &RangeSlider::hoverLeft);
    QEvent leave(QEvent::Leave);
    QApplication::sendEvent(m_slider, &leave);
    QCOMPARE(spy.count(), 1);
}

void TestRangeSlider::dragStarted_firesOnFirstMoveAfterPress()
{
    QSignalSpy startSpy(m_slider, &RangeSlider::dragStarted);
    QSignalSpy endSpy  (m_slider, &RangeSlider::dragEnded);

    // 単純クリック（press → release のみ）では dragStarted は発火しない
    pressLeft  (m_slider, 50);
    releaseLeft(m_slider, 50);
    QCOMPARE(startSpy.count(), 0);
    QCOMPARE(endSpy.count(),   0);

    // press → move → release で dragStarted / dragEnded が各 1 回
    pressLeft  (m_slider, 50);
    moveLeft   (m_slider, 100);
    moveLeft   (m_slider, 120);
    releaseLeft(m_slider, 120);
    QCOMPARE(startSpy.count(), 1);
    QCOMPARE(endSpy.count(),   1);
}

QTEST_MAIN(TestRangeSlider)
#include "test_RangeSlider.moc"
