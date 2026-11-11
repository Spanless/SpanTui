#include "application/icon_motion.h"

#include "application/application_constants.h"
#include "application/ui_icon_catalog.h"

#include <QElapsedTimer>
#include <QJsonArray>
#include <QJsonObject>
#include <QJsonValue>
#include <QRegularExpression>
#include <QVariantMap>

#include <algorithm>
#include <cmath>

namespace {

using Property = IconMotionSpec::Property;
using Easing = IconMotionSpec::Easing;

constexpr int PropertySlots = 16;

int keyOf(int part, Property property) {
    return part * PropertySlots + static_cast<int>(property);
}

const QRegularExpression &numberPattern() {
    static const QRegularExpression pattern(QStringLiteral("-?\\d*\\.?\\d+"));
    return pattern;
}

bool tokenize(const QString &path, QStringList *separators, QList<double> *numbers) {
    separators->clear();
    numbers->clear();
    qsizetype last = 0;
    auto it = numberPattern().globalMatch(path);
    while (it.hasNext()) {
        const auto match = it.next();
        separators->append(path.mid(last, match.capturedStart() - last));
        bool ok = false;
        numbers->append(match.captured().toDouble(&ok));
        if (!ok) {
            return false;
        }
        last = match.capturedEnd();
    }
    separators->append(path.mid(last));
    return !numbers->isEmpty();
}

double bezier(double a, double b, double t) {
    const double u = 1 - t;
    return 3 * a * u * u * t + 3 * b * u * t * t + t * t * t;
}

// CSS-style cubic-bezier(x1, y1, x2, y2); y may overshoot for Back.
double ease(Easing easing, double x) {
    struct Curve {
        double x1, y1, x2, y2;
    };
    Curve c;
    switch (easing) {
    case Easing::Linear:
        return x;
    case Easing::In:
        c = {0.5, 0, 0.75, 0};
        break;
    case Easing::Out:
        c = {0.22, 1, 0.36, 1};
        break;
    case Easing::InOut:
        c = {0.65, 0, 0.35, 1};
        break;
    case Easing::Back:
        c = {0.34, 1.56, 0.64, 1};
        break;
    }
    double lo = 0;
    double hi = 1;
    double t = x;
    for (int i = 0; i < 24; ++i) {
        t = (lo + hi) / 2;
        if (bezier(c.x1, c.x2, t) < x) {
            lo = t;
        } else {
            hi = t;
        }
    }
    return bezier(c.y1, c.y2, t);
}

double easeOut(double x) {
    return ease(Easing::Out, x);
}

double sampleTrack(const IconMotionSpec::Track &track, double ms) {
    const auto &keys = track.keys;
    double t = (ms - track.delay) / track.duration;
    if (t < 0) {
        return keys.first().value;
    }
    if (track.loop) {
        t = std::fmod(t, 1.0);
    } else if (t >= 1) {
        return keys.last().value;
    }
    for (qsizetype i = 1; i < keys.size(); ++i) {
        if (t <= keys[i].t) {
            const auto &a = keys[i - 1];
            const auto &b = keys[i];
            if (b.t <= a.t) {
                return b.value;
            }
            return a.value + (b.value - a.value) * ease(b.easing, (t - a.t) / (b.t - a.t));
        }
    }
    return keys.last().value;
}

bool propertyFromName(const QString &name, Property *out) {
    static const QHash<QString, Property> names{
        {QStringLiteral("rotate"), Property::Rotate},
        {QStringLiteral("tx"), Property::TranslateX},
        {QStringLiteral("ty"), Property::TranslateY},
        {QStringLiteral("scale"), Property::Scale},
        {QStringLiteral("sx"), Property::ScaleX},
        {QStringLiteral("sy"), Property::ScaleY},
        {QStringLiteral("opacity"), Property::Opacity},
        {QStringLiteral("draw"), Property::Draw},
        {QStringLiteral("morph"), Property::Morph},
        {QStringLiteral("tint"), Property::Tint},
        {QStringLiteral("fill"), Property::Fill},
    };
    const auto it = names.constFind(name);
    if (it == names.cend()) {
        return false;
    }
    *out = *it;
    return true;
}

bool easingFromName(const QString &name, Easing *out) {
    static const QHash<QString, Easing> names{
        {QStringLiteral("l"), Easing::Linear},
        {QStringLiteral("i"), Easing::In},
        {QStringLiteral("o"), Easing::Out},
        {QStringLiteral("io"), Easing::InOut},
        {QStringLiteral("back"), Easing::Back},
    };
    const auto it = names.constFind(name);
    if (it == names.cend()) {
        return false;
    }
    *out = *it;
    return true;
}

bool isTintRole(const QString &role) {
    static const QSet<QString> roles{QStringLiteral("accent"), QStringLiteral("danger"),
                                     QStringLiteral("success"), QStringLiteral("warning"),
                                     QStringLiteral("cursor")};
    return roles.contains(role);
}

bool fail(QString *error, const QString &reason) {
    if (error) {
        *error = reason;
    }
    return false;
}

// Three decimals without trailing zeros. Arc flags are path numbers too, and
// Qt reads a flag as one digit: "1.000" would shift every later argument and
// the whole path would fail to parse.
QString compactNumber(double value)
{
    QString text = QString::number(value, 'f', 3);
    while (text.endsWith(QLatin1Char('0'))) text.chop(1);
    if (text.endsWith(QLatin1Char('.'))) text.chop(1);
    return text == QLatin1String("-0") ? QStringLiteral("0") : text;
}

} // namespace

QString IconMotionSpec::Part::morphPath(double t) const {
    QString out = morphSeparators.value(0);
    for (qsizetype i = 0; i < morphFrom.size(); ++i) {
        out += compactNumber(morphFrom[i] + (morphTo[i] - morphFrom[i]) * t);
        out += morphSeparators.value(i + 1);
    }
    return out;
}

double IconMotionSpec::layerLength(Layer layer) const {
    double length = 0;
    for (const Track &track : layers[layer]) {
        length = std::max(length, track.delay + track.duration);
    }
    return length;
}

bool IconMotionSpec::parse(const QJsonObject &json, const QHash<QString, QString> &paths,
                           IconMotionSpec *out, QString *error) {
    IconMotionSpec spec;
    QHash<QString, int> index;

    const QJsonArray parts = json.value(QStringLiteral("parts")).toArray();
    if (parts.isEmpty()) {
        return fail(error, QStringLiteral("no parts"));
    }
    for (const QJsonValue &value : parts) {
        const QJsonObject object = value.toObject();
        Part part;
        part.id = object.value(QStringLiteral("id")).toString();
        if (part.id.isEmpty() || index.contains(part.id)) {
            return fail(error, QStringLiteral("missing or duplicate part id '%1'").arg(part.id));
        }
        const QString pathId = object.value(QStringLiteral("path")).toString();
        if (!pathId.isEmpty()) {
            if (!paths.contains(pathId)) {
                return fail(error, QStringLiteral("part '%1' names unknown path '%2'").arg(part.id, pathId));
            }
            part.path = paths.value(pathId);
        }
        const QString morphId = object.value(QStringLiteral("morph")).toString();
        if (!morphId.isEmpty()) {
            QStringList targetSeparators;
            if (part.path.isEmpty() || !paths.contains(morphId)
                || !tokenize(part.path, &part.morphSeparators, &part.morphFrom)
                || !tokenize(paths.value(morphId), &targetSeparators, &part.morphTo)
                || targetSeparators != part.morphSeparators) {
                return fail(error, QStringLiteral("part '%1' cannot morph to '%2'").arg(part.id, morphId));
            }
        }
        const QJsonArray origin = object.value(QStringLiteral("origin")).toArray();
        if (origin.size() != 2) {
            return fail(error, QStringLiteral("part '%1' needs an [x, y] origin").arg(part.id));
        }
        part.origin = QPointF(origin.at(0).toDouble(), origin.at(1).toDouble());
        const QString parent = object.value(QStringLiteral("parent")).toString();
        if (!parent.isEmpty()) {
            if (!index.contains(parent)) {
                return fail(error, QStringLiteral("part '%1' must follow its parent '%2'").arg(part.id, parent));
            }
            part.parent = index.value(parent);
        }
        part.solid = object.value(QStringLiteral("solid")).toBool();
        part.tint = object.value(QStringLiteral("tint")).toString();
        if (!part.tint.isEmpty() && !isTintRole(part.tint)) {
            return fail(error, QStringLiteral("part '%1' has unknown tint '%2'").arg(part.id, part.tint));
        }
        const QJsonObject rest = object.value(QStringLiteral("rest")).toObject();
        part.restOpacity = rest.value(QStringLiteral("opacity")).toDouble(1);
        part.restFill = rest.value(QStringLiteral("fill")).toDouble(1);
        part.wrap = object.value(QStringLiteral("wrap")).toDouble(0);
        part.length = object.value(QStringLiteral("length")).toDouble(0);
        index.insert(part.id, int(spec.parts.size()));
        spec.parts.append(part);
    }

    static const std::array<QString, LayerCount> layerNames{
        QStringLiteral("hover"), QStringLiteral("press"), QStringLiteral("active")};
    for (int layer = 0; layer < LayerCount; ++layer) {
        const QJsonArray tracks = json.value(layerNames[layer]).toArray();
        for (const QJsonValue &value : tracks) {
            const QJsonObject object = value.toObject();
            Track track;
            const QString partId = object.value(QStringLiteral("part")).toString();
            if (!index.contains(partId)) {
                return fail(error, QStringLiteral("%1 track names unknown part '%2'").arg(layerNames[layer], partId));
            }
            track.part = index.value(partId);
            const Part &part = spec.parts[track.part];
            const QString prop = object.value(QStringLiteral("prop")).toString();
            if (!propertyFromName(prop, &track.property)) {
                return fail(error, QStringLiteral("unknown property '%1'").arg(prop));
            }
            track.duration = object.value(QStringLiteral("dur")).toDouble(0);
            track.delay = object.value(QStringLiteral("delay")).toDouble(0);
            track.loop = object.value(QStringLiteral("loop")).toBool();
            if (track.duration <= 0 || track.delay < 0) {
                return fail(error, QStringLiteral("%1.%2 needs a positive duration").arg(partId, prop));
            }
            double lastT = 0;
            for (const QJsonValue &keyValue : object.value(QStringLiteral("keys")).toArray()) {
                const QJsonArray k = keyValue.toArray();
                Key key;
                key.t = k.at(0).toDouble(-1);
                key.value = k.at(1).toDouble();
                if (k.size() < 2 || key.t < lastT || key.t > 1) {
                    return fail(error, QStringLiteral("%1.%2 has an out-of-order key").arg(partId, prop));
                }
                if (k.size() > 2 && !easingFromName(k.at(2).toString(), &key.easing)) {
                    return fail(error, QStringLiteral("%1.%2 has an unknown easing").arg(partId, prop));
                }
                lastT = key.t;
                track.keys.append(key);
            }
            if (track.keys.isEmpty()) {
                return fail(error, QStringLiteral("%1.%2 has no keys").arg(partId, prop));
            }
            if (track.property == Property::Draw && (part.length <= 0 || part.solid)) {
                return fail(error, QStringLiteral("%1 draws without a stroke length").arg(partId));
            }
            if (track.property == Property::Morph && !part.morphs()) {
                return fail(error, QStringLiteral("%1 morphs without a target").arg(partId));
            }
            if (track.loop && layer != Hover) {
                return fail(error, QStringLiteral("only hover tracks may loop (%1.%2)").arg(partId, prop));
            }
            if (layer == Press) {
                const double end = track.keys.last().value;
                const bool wrapped = track.property == Property::Rotate && part.wrap > 0
                                     && std::abs(end / part.wrap - std::round(end / part.wrap)) < 1e-6;
                if (end != 0 && !wrapped) {
                    return fail(error, QStringLiteral("press track %1.%2 must end at rest").arg(partId, prop));
                }
            }
            spec.layers[layer].append(track);
        }
    }
    *out = spec;
    return true;
}

IconMotionPlayer::IconMotionPlayer(const IconMotionSpec *spec) : m_spec(spec) {
    if (m_spec) {
        for (const auto &track : m_spec->layers[IconMotionSpec::Active]) {
            m_activeKeys.insert(keyOf(track.part, track.property));
        }
    }
}

void IconMotionPlayer::setReducedMotion(bool reduced) {
    if (m_reduced == reduced) {
        return;
    }
    m_reduced = reduced;
    if (!reduced) {
        return;
    }
    m_layers[IconMotionSpec::Hover] = {};
    m_layers[IconMotionSpec::Press] = {};
    auto &active = m_layers[IconMotionSpec::Active];
    active.from.clear();
    active.residual.clear();
    if (active.running) {
        active.start = -1e12;
    }
}

void IconMotionPlayer::setHovered(bool hovered, double now) {
    if (!m_spec || (hovered && m_reduced)) {
        return;
    }
    hovered ? turnOn(IconMotionSpec::Hover, now) : turnOff(IconMotionSpec::Hover, now);
}

void IconMotionPlayer::setActive(bool active, double now, bool instant) {
    if (!m_spec) {
        return;
    }
    if (active) {
        turnOn(IconMotionSpec::Active, now);
        if (instant || m_reduced) {
            auto &state = m_layers[IconMotionSpec::Active];
            state.start = now - m_spec->layerLength(IconMotionSpec::Active) - 1;
            state.residual.clear();
        }
    } else {
        turnOff(IconMotionSpec::Active, now);
        if (instant || m_reduced) {
            m_layers[IconMotionSpec::Active].from.clear();
        }
    }
}

void IconMotionPlayer::press(double now) {
    if (!m_spec || m_reduced || m_spec->layers[IconMotionSpec::Press].isEmpty()) {
        return;
    }
    turnOn(IconMotionSpec::Press, now);
}

double IconMotionPlayer::settleMs() const {
    return m_reduced ? 0 : span::application::kIconMotionSettleMs;
}

double IconMotionPlayer::wrapOf(int key) const {
    if (key % PropertySlots != static_cast<int>(Property::Rotate)) {
        return 0;
    }
    return m_spec->parts[key / PropertySlots].wrap;
}

QHash<int, double> IconMotionPlayer::normalized(const QHash<int, double> &values) const {
    QHash<int, double> out;
    for (auto it = values.cbegin(); it != values.cend(); ++it) {
        const double wrap = wrapOf(it.key());
        out.insert(it.key(), wrap > 0 ? it.value() - std::round(it.value() / wrap) * wrap : it.value());
    }
    return out;
}

QHash<int, double> IconMotionPlayer::sample(int layer, double now) {
    LayerState &state = m_layers[layer];
    QHash<int, double> values;
    const double settle = settleMs();
    if (state.running) {
        const double ms = now - state.start;
        if (layer == IconMotionSpec::Press && ms >= m_spec->layerLength(IconMotionSpec::Press)) {
            state.running = false;
        } else {
            for (const auto &track : m_spec->layers[layer]) {
                if (track.loop && m_reduced) {
                    continue;
                }
                values[keyOf(track.part, track.property)] += sampleTrack(track, ms);
            }
        }
    } else if (!state.from.isEmpty()) {
        const double p = settle > 0 ? std::min(1.0, (now - state.fromAt) / settle) : 1.0;
        if (p >= 1) {
            state.from.clear();
        } else {
            const double e = easeOut(p);
            for (auto it = state.from.cbegin(); it != state.from.cend(); ++it) {
                const double wrap = wrapOf(it.key());
                const double target = wrap > 0 ? std::round(it.value() / wrap) * wrap : 0;
                values[it.key()] = it.value() + (target - it.value()) * e;
            }
        }
    }
    if (!state.residual.isEmpty()) {
        const double p = settle > 0 ? std::min(1.0, (now - state.residualAt) / settle) : 1.0;
        if (p >= 1) {
            state.residual.clear();
        } else {
            const double keep = 1 - easeOut(p);
            for (auto it = state.residual.cbegin(); it != state.residual.cend(); ++it) {
                values[it.key()] += it.value() * keep;
            }
        }
    }
    return values;
}

void IconMotionPlayer::turnOn(int layer, double now) {
    const QHash<int, double> current = sample(layer, now);
    LayerState &state = m_layers[layer];
    state.residual = normalized(current);
    state.residualAt = now;
    state.from.clear();
    state.running = true;
    state.start = now;
}

void IconMotionPlayer::turnOff(int layer, double now) {
    if (layer == IconMotionSpec::Press) {
        return;
    }
    const QHash<int, double> current = sample(layer, now);
    LayerState &state = m_layers[layer];
    state.running = false;
    state.residual.clear();
    state.from = current;
    state.fromAt = now;
}

double IconMotionPlayer::activeWeight(double now) const {
    const LayerState &state = m_layers[IconMotionSpec::Active];
    if (state.running) {
        const double length = m_spec->layerLength(IconMotionSpec::Active);
        return length > 0 ? std::clamp((now - state.start) / length, 0.0, 1.0) : 1.0;
    }
    if (!state.from.isEmpty()) {
        const double settle = settleMs();
        return settle > 0 ? 1 - easeOut(std::min(1.0, (now - state.fromAt) / settle)) : 0.0;
    }
    return 0;
}

QList<IconMotionFrame> IconMotionPlayer::evaluate(double now) {
    QList<IconMotionFrame> frames;
    if (!m_spec) {
        return frames;
    }
    const double weight = activeWeight(now);
    QHash<int, double> sum;
    for (int layer = 0; layer < IconMotionSpec::LayerCount; ++layer) {
        const QHash<int, double> values = sample(layer, now);
        for (auto it = values.cbegin(); it != values.cend(); ++it) {
            const bool silenced = layer == IconMotionSpec::Hover && m_activeKeys.contains(it.key());
            sum[it.key()] += silenced ? it.value() * (1 - weight) : it.value();
        }
    }
    frames.reserve(m_spec->parts.size());
    for (int i = 0; i < m_spec->parts.size(); ++i) {
        const auto &part = m_spec->parts[i];
        const auto get = [&](Property property) { return sum.value(keyOf(i, property)); };
        const double scale = 1 + get(Property::Scale);
        const double ox = part.origin.x();
        const double oy = part.origin.y();

        IconMotionFrame frame;
        QMatrix4x4 local;
        local.translate(float(get(Property::TranslateX) + ox), float(get(Property::TranslateY) + oy));
        local.rotate(float(get(Property::Rotate)), 0, 0, 1);
        local.scale(float(scale * (1 + get(Property::ScaleX))), float(scale * (1 + get(Property::ScaleY))));
        local.translate(float(-ox), float(-oy));
        double opacity = std::clamp(part.restOpacity + get(Property::Opacity), 0.0, 1.0);
        if (part.parent >= 0) {
            frame.matrix = frames[part.parent].matrix * local;
            opacity *= frames[part.parent].opacity;
        } else {
            frame.matrix = local;
        }
        frame.opacity = opacity;
        frame.draw = std::clamp(1 + get(Property::Draw), 0.0, 1.0);
        frame.tint = std::clamp(get(Property::Tint), 0.0, 1.0);
        frame.fill = std::clamp(part.restFill + get(Property::Fill), 0.0, 1.0);
        frame.morph = get(Property::Morph);
        frames.append(frame);
    }
    return frames;
}

bool IconMotionPlayer::layerSettled(int layer, double now) const {
    const LayerState &state = m_layers[layer];
    const double settle = settleMs();
    if (!state.from.isEmpty() && now < state.fromAt + settle) {
        return false;
    }
    if (!state.residual.isEmpty() && now < state.residualAt + settle) {
        return false;
    }
    if (!state.running) {
        return true;
    }
    if (now < state.start + m_spec->layerLength(IconMotionSpec::Layer(layer))) {
        return false;
    }
    if (layer == IconMotionSpec::Hover && !m_reduced) {
        for (const auto &track : m_spec->layers[layer]) {
            if (track.loop) {
                return false;
            }
        }
    }
    return true;
}

bool IconMotionPlayer::settled(double now) const {
    if (!m_spec) {
        return true;
    }
    for (int layer = 0; layer < IconMotionSpec::LayerCount; ++layer) {
        if (!layerSettled(layer, now)) {
            return false;
        }
    }
    return true;
}

bool IconMotionPlayer::atRest(double now) const {
    if (!settled(now)) {
        return false;
    }
    if (!m_spec) {
        return true;
    }
    // A finished press no longer counts; held layers do.
    return !m_layers[IconMotionSpec::Hover].running && !m_layers[IconMotionSpec::Active].running;
}

// Ticks with the animation timer, so it samples once per frame, but reads
// the wall clock: Qt Quick's default driver advances its time by one vsync
// of the primary screen per frame, so a window rendering faster than that
// (a higher-refresh monitor) would play every motion faster.
class IconMotion::Clock final : public QAbstractAnimation {
public:
    explicit Clock(IconMotion *owner) : QAbstractAnimation(owner), m_owner(owner) {}
    int duration() const override { return -1; }

protected:
    void updateState(State newState, State oldState) override
    {
        if (newState == Running && oldState == Stopped) {
            m_wall.start();
        }
    }
    void updateCurrentTime(int) override { m_owner->tick(m_owner->m_clockBase + double(m_wall.nsecsElapsed()) / 1e6); }

private:
    IconMotion *m_owner;
    QElapsedTimer m_wall;
};

IconMotion::IconMotion(QObject *parent) : QObject(parent), m_clock(new Clock(this)) {}

IconMotion::~IconMotion() = default;

void IconMotion::classBegin() {
    m_complete = false;
}

void IconMotion::componentComplete() {
    m_complete = true;
    reload();
}

UiIconCatalog *IconMotion::catalog() const
{
    return m_catalog;
}

void IconMotion::setCatalog(UiIconCatalog *catalog)
{
    if (m_catalog == catalog) {
        return;
    }
    m_catalog = catalog;
    emit catalogChanged();
    reload();
}

QString IconMotion::name() const {
    return m_name;
}

void IconMotion::setName(const QString &name) {
    if (m_name == name) {
        return;
    }
    m_name = name;
    emit nameChanged();
    reload();
}

bool IconMotion::hovered() const {
    return m_hovered;
}

void IconMotion::setHovered(bool hovered) {
    if (m_hovered == hovered) {
        return;
    }
    m_hovered = hovered;
    emit hoveredChanged();
    if (m_complete && m_spec) {
        m_player.setHovered(hovered, m_time);
        refresh();
    }
}

bool IconMotion::active() const {
    return m_active;
}

void IconMotion::setActive(bool active) {
    if (m_active == active) {
        return;
    }
    m_active = active;
    emit activeChanged();
    if (m_complete && m_spec) {
        m_player.setActive(active, m_time);
        refresh();
    }
}

bool IconMotion::reduceMotion() const {
    return m_reduce;
}

void IconMotion::setReduceMotion(bool reduce) {
    if (m_reduce == reduce) {
        return;
    }
    m_reduce = reduce;
    emit reduceMotionChanged();
    m_player.setReducedMotion(reduce);
    if (m_complete) {
        refresh();
    }
}

bool IconMotion::engaged() const {
    return m_engaged;
}

QVariantList IconMotion::parts() const {
    return m_parts;
}

QVariantList IconMotion::frame() const {
    return m_frame;
}

void IconMotion::press() {
    if (!m_complete || !m_spec) {
        return;
    }
    m_player.press(m_time);
    refresh();
}

void IconMotion::reload() {
    if (!m_complete) {
        return;
    }
    m_clock->stop();
    m_spec = m_catalog ? m_catalog->motion(m_name) : nullptr;
    m_player = IconMotionPlayer(m_spec);
    m_player.setReducedMotion(m_reduce);
    // A fresh icon shows the state it was created in; only later changes move.
    m_player.setActive(m_active, m_time, true);

    m_parts.clear();
    if (m_spec) {
        for (const auto &part : m_spec->parts) {
            m_parts.append(QVariantMap{
                {QStringLiteral("path"), part.path},
                {QStringLiteral("solid"), part.solid},
                {QStringLiteral("tint"), part.tint},
                {QStringLiteral("length"), part.length},
                {QStringLiteral("morph"), part.morphs()},
            });
        }
    }
    emit partsChanged();
    refresh();
}

void IconMotion::tick(double now) {
    m_time = now;
    refresh();
}

void IconMotion::refresh() {
    if (m_refreshing) {
        return;
    }
    m_refreshing = true;
    const QList<IconMotionFrame> frames = m_player.evaluate(m_time);
    m_frame.clear();
    m_frame.reserve(frames.size());
    for (qsizetype i = 0; i < frames.size(); ++i) {
        const IconMotionFrame &f = frames[i];
        const auto &part = m_spec->parts[i];
        m_frame.append(QVariantMap{
            {QStringLiteral("matrix"), f.matrix},
            {QStringLiteral("opacity"), f.opacity},
            {QStringLiteral("draw"), f.draw},
            {QStringLiteral("tint"), f.tint},
            {QStringLiteral("fill"), f.fill},
            {QStringLiteral("path"), part.morphs() ? part.morphPath(f.morph) : part.path},
        });
    }
    emit frameChanged();

    const bool engaged = m_spec && !m_player.atRest(m_time);
    if (engaged != m_engaged) {
        m_engaged = engaged;
        emit engagedChanged();
    }
    if (m_player.settled(m_time)) {
        m_clock->stop();
    } else if (m_clock->state() != QAbstractAnimation::Running) {
        m_clockBase = m_time;
        m_clock->start();
    }
    m_refreshing = false;
}
