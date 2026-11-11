#pragma once

#include <QAbstractAnimation>
#include <QHash>
#include <QList>
#include <QMatrix4x4>
#include <QObject>
#include <QPointF>
#include <QPointer>
#include <QQmlParserStatus>
#include <QSet>
#include <QString>
#include <QStringList>
#include <QVariantList>

#include <array>

class QJsonObject;
class UiIconCatalog;

// Motion of one animated icon: its parts and three keyframe layers, loaded
// from <name>.motion.json next to the SVG. Format and design rules:
// docs/ui/icons.md.
struct IconMotionSpec {
    enum class Property { Rotate, TranslateX, TranslateY, Scale, ScaleX, ScaleY, Opacity, Draw, Morph, Tint, Fill };
    enum class Easing { Linear, In, Out, InOut, Back };
    enum Layer { Hover, Press, Active, LayerCount };

    struct Key {
        double t = 0;
        double value = 0;
        Easing easing = Easing::InOut;
    };
    struct Track {
        int part = 0;
        Property property = Property::Rotate;
        QList<Key> keys;
        double duration = 1;
        double delay = 0;
        bool loop = false;
    };
    struct Part {
        QString id;
        QString path;          // SVG path data; empty for a pure transform group
        int parent = -1;       // always an earlier part
        QPointF origin;
        bool solid = false;    // filled instead of stroked
        QString tint;          // Theme role the part leans towards; empty = none
        double restOpacity = 1;
        double restFill = 1;
        double wrap = 0;       // rotation period at which the part looks unchanged
        double length = 0;     // stroke length for Draw (dash trim)
        QStringList morphSeparators;
        QList<double> morphFrom;
        QList<double> morphTo;

        bool morphs() const { return !morphFrom.isEmpty(); }
        QString morphPath(double t) const;
    };

    QList<Part> parts;
    std::array<QList<Track>, LayerCount> layers;

    // `paths` maps the SVG's path ids to their data. Fails closed with a
    // reason in `error`.
    static bool parse(const QJsonObject &json, const QHash<QString, QString> &paths,
                      IconMotionSpec *out, QString *error);
    // Longest delay + duration in a layer.
    double layerLength(Layer layer) const;
};

// Resolved per-part values for one instant.
struct IconMotionFrame {
    QMatrix4x4 matrix;     // design-grid (24x24) transform, parents applied
    double opacity = 1;    // parents applied
    double draw = 1;       // visible fraction of the stroke, from its start
    double tint = 0;
    double fill = 1;
    double morph = 0;
};

// Deterministic evaluator; callers pass time in milliseconds. Layers add up:
// hover is held while hovered, press is a one-shot, active is a static pose
// reached by its tracks. Properties the active layer animates silence hover
// while active. Leaving a held layer settles its values back to rest.
class IconMotionPlayer {
public:
    explicit IconMotionPlayer(const IconMotionSpec *spec = nullptr);

    // Reduced motion drops hover and press and makes active switch instantly.
    void setReducedMotion(bool reduced);
    void setHovered(bool hovered, double now);
    // `instant` lands on the final pose (initial state, reduced motion).
    void setActive(bool active, double now, bool instant = false);
    void press(double now);

    QList<IconMotionFrame> evaluate(double now);
    // Nothing changes after `now` until the next input.
    bool settled(double now) const;
    // Every value is at rest: the plain static glyph is equivalent.
    bool atRest(double now) const;

private:
    struct LayerState {
        bool running = false;
        double start = 0;
        QHash<int, double> from;
        double fromAt = 0;
        QHash<int, double> residual;
        double residualAt = 0;
    };

    QHash<int, double> sample(int layer, double now);
    QHash<int, double> normalized(const QHash<int, double> &values) const;
    double wrapOf(int key) const;
    double activeWeight(double now) const;
    double settleMs() const;
    bool layerSettled(int layer, double now) const;
    void turnOn(int layer, double now);
    void turnOff(int layer, double now);

    const IconMotionSpec *m_spec = nullptr;
    std::array<LayerState, IconMotionSpec::LayerCount> m_layers;
    QSet<int> m_activeKeys;
    bool m_reduced = false;
};

// QML face of IconMotionPlayer, ticked by the animation timer and timed by
// the wall clock, so motion speed does not depend on the frame rate. Idle
// icons do not tick: the clock runs only while some layer still moves. The
// state an instance is created with applies without animation.
class IconMotion : public QObject, public QQmlParserStatus {
    Q_OBJECT
    Q_INTERFACES(QQmlParserStatus)
    Q_MOC_INCLUDE("application/ui_icon_catalog.h")
    // The `UiIcons` singleton.
    Q_PROPERTY(UiIconCatalog *catalog READ catalog WRITE setCatalog NOTIFY catalogChanged FINAL)
    Q_PROPERTY(QString name READ name WRITE setName NOTIFY nameChanged FINAL)
    Q_PROPERTY(bool hovered READ hovered WRITE setHovered NOTIFY hoveredChanged FINAL)
    Q_PROPERTY(bool active READ active WRITE setActive NOTIFY activeChanged FINAL)
    Q_PROPERTY(bool reduceMotion READ reduceMotion WRITE setReduceMotion NOTIFY reduceMotionChanged FINAL)
    Q_PROPERTY(bool engaged READ engaged NOTIFY engagedChanged FINAL)
    Q_PROPERTY(QVariantList parts READ parts NOTIFY partsChanged FINAL)
    Q_PROPERTY(QVariantList frame READ frame NOTIFY frameChanged FINAL)

public:
    explicit IconMotion(QObject *parent = nullptr);
    ~IconMotion() override;

    UiIconCatalog *catalog() const;
    void setCatalog(UiIconCatalog *catalog);
    QString name() const;
    void setName(const QString &name);
    bool hovered() const;
    void setHovered(bool hovered);
    bool active() const;
    void setActive(bool active);
    bool reduceMotion() const;
    void setReduceMotion(bool reduce);

    // True while the icon differs from its static glyph.
    bool engaged() const;
    // Static per-part descriptors: path, solid, tint, length.
    QVariantList parts() const;
    // Per-part values for the current instant: matrix, opacity, draw, tint,
    // fill, path.
    QVariantList frame() const;

    Q_INVOKABLE void press();

    void classBegin() override;
    void componentComplete() override;

signals:
    void catalogChanged();
    void nameChanged();
    void hoveredChanged();
    void activeChanged();
    void reduceMotionChanged();
    void engagedChanged();
    void partsChanged();
    void frameChanged();

private:
    class Clock;
    friend class Clock;

    void reload();
    void tick(double now);
    void refresh();

    QPointer<UiIconCatalog> m_catalog;
    bool m_complete = true;
    bool m_refreshing = false;
    QString m_name;
    bool m_hovered = false;
    bool m_active = false;
    bool m_reduce = false;
    bool m_engaged = false;
    const IconMotionSpec *m_spec = nullptr;
    IconMotionPlayer m_player;
    QVariantList m_parts;
    QVariantList m_frame;
    Clock *m_clock = nullptr;
    double m_time = 0;
    double m_clockBase = 0;
};
