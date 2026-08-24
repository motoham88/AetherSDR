#pragma once

#include <QString>
#include <QStringList>

namespace AetherSDR {

// ── The one list of radio families the app can dial ────────────────────────
//
// This exists because the set was previously spelled out inline in FOUR
// places — two normalizers and a validator in ConnectionPanel, and the
// `connect ip` verb in AutomationServer — which had already drifted apart in
// behaviour. Adding a family meant finding all four, and the automation verb
// is the one that was easiest to miss: a family could be fully wired through
// the GUI, the factory and the backend, and still be rejected by the bridge
// with "radio type must be flex, hl2 or icom".
//
// That is the same "find every list" failure mode IRadioBackend.h documents
// for `m_family != "flex"` checks, and the same reason RadioCapabilities asks
// a backend what it can do instead of testing its name.
//
// IN CORE, not gui, precisely so AutomationServer can use it: the automation
// seam lives below the GUI and must not include a panel header to learn what
// a valid family is.
//
// ADDING A FAMILY: add it here, add the backend branch in
// RadioModel::makeBackend, and add the picker entry in ConnectionPanel. This
// header is what keeps the first of those from being three separate edits.
namespace RadioFamilies {

inline constexpr const char* kFlex = "flex";
inline constexpr const char* kHl2  = "hl2";
inline constexpr const char* kIcom = "icom";
inline constexpr const char* kTci  = "tci";

// Order is the order the manual radio-type selector offers them.
inline QStringList all()
{
    return {QString::fromLatin1(kFlex),
            QString::fromLatin1(kHl2),
            QString::fromLatin1(kIcom),
            QString::fromLatin1(kTci)};
}

[[nodiscard]] inline bool isKnown(const QString& family)
{
    return all().contains(family.trimmed().toLower());
}

// Canonical spelling, or flex for anything unrecognised — which is what a
// profile written before the selector existed actually was.
//
// NOT for validating operator or automation input: a caller that names a
// family we do not have must hear so rather than be quietly connected to a
// Flex. Use isKnown() for that.
[[nodiscard]] inline QString normalize(const QString& family)
{
    const QString lowered = family.trimmed().toLower();
    return isKnown(lowered) ? lowered : QString::fromLatin1(kFlex);
}

// Human-readable list for an error message: "flex, hl2, icom or tci".
[[nodiscard]] inline QString describeAll()
{
    const QStringList families = all();
    if (families.size() < 2) return families.join(QString());
    return QStringLiteral("%1 or %2")
        .arg(QStringList(families.mid(0, families.size() - 1)).join(QStringLiteral(", ")),
             families.last());
}

}  // namespace RadioFamilies
}  // namespace AetherSDR
