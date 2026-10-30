// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 KC2RGW Chris

.pragma library

// What an effect's own meters (DaemonClient.effectMeters) say about its gain,
// for the effects window's meter bridge and the mixer's insert slots.

// The gain reduction in dB (0 or more), or null for a type that has none:
// the gate's cut, the ducker's dip, the limiting, the multiband
// compressor's deepest band, the leveler's cut.
function gainReduction(type, values) {
    const v = values || [];
    switch (type) {
    case "gate":
    case "ducker":
    case "limiter":
        return v.length > 0 ? Math.max(0, v[0]) : 0;
    case "multiband":
        return v.length > 0 ? Math.max(0, ...v) : 0;
    case "leveler":
        return v.length > 0 ? Math.max(0, -v[0]) : 0;
    }
    return null;
}

// The dB a full gain-reduction meter stands for.
function reductionRange(type) {
    return type === "gate" ? 40 : type === "limiter" ? 12 : 24;
}

// The leveler both boosts and cuts: its meter is centred on 0 dB.
function isBipolar(type) {
    return type === "leveler";
}
