/*
 // Copyright (c) 2026 PlugData MCP Team
 // For information on usage and redistribution, and for a DISCLAIMER OF ALL
 // WARRANTIES, see the file, "LICENSE.txt," in this distribution.
*/

// batch_atomic stage 2, phase 5 (P3.4 S9): connect with the rate and cycle guards, then the
// post-mutation wire scan for the receipt. Pd context (message thread, sys_lock).

#include "MCPBridgeCmdBatch.h"

void mcp::BatchJob::connectPhase()
{
    // PHASE 5: CONNECT via obj_connect
    // R2 — live signal adjacency for cycle detection (PRD preflight).
    // Built once before the loop: signal edges only. Bounded: skip
    // if graph too large (2000 objs / 4000 wires) — X-ray still reports.
    std::unordered_map<t_gobj*, std::vector<t_gobj*>> liveSigAdj;
    std::unordered_map<t_gobj*, std::vector<t_gobj*>> tentativeSigAdj;
    bool cycleGuardActive = false;
    {
        int liveObjCount = 0;
        for (t_gobj* y = cnv->gl_list; y; y = y->g_next) liveObjCount++;
        if (liveObjCount <= 2000) {
            t_linetraverser ltCount;
            linetraverser_start(&ltCount, cnv);
            int wireCount = 0;
            while (linetraverser_next_nosize(&ltCount)) wireCount++;
            if (wireCount <= 4000) {
                cycleGuardActive = true;
                t_linetraverser lt;
                linetraverser_start(&lt, cnv);
                t_outconnect* ocTmp = nullptr;
                while ((ocTmp = linetraverser_next_nosize(&lt))) {
                    t_gobj* sgTmp = &lt.tr_ob->ob_g;
                    t_gobj* dgTmp = &lt.tr_ob2->ob_g;
                    t_object* soTmp = pd::Interface::checkObject(sgTmp);
                    if (soTmp && obj_issignaloutlet(soTmp, lt.tr_outno)) {
                        liveSigAdj[sgTmp].push_back(dgTmp);
                    }
                }
                tentativeSigAdj = liveSigAdj;
            }
        }
    }
    for (auto& cc : allConns) {
        t_gobj* sg = processor->getMcp()->resolveStableId(canvasName, cc.srcId);
        t_gobj* dg = processor->getMcp()->resolveStableId(canvasName, cc.destId);
        if (!sg || !dg) {
            // Phase A (PRD diagnostic layer): name the silent
            // failure instead of skipping silently.
            std::string reason;
            if (!sg && !dg) reason = "src and dest not found";
            else if (!sg)   reason = "src not found";
            else            reason = "dest not found";
            connectFailures.push_back({
                cc.srcId.toStdString(), cc.destId.toStdString(), reason });
            continue;
        }
        t_object* so = pd::Interface::checkObject(sg);
        t_object* d_o = pd::Interface::checkObject(dg);
        if (so && d_o) {
            // Phase B (fault-injection gauntlet 2026-09-09):
            // name self-connections, duplicate wires, and
            // out-of-range ports instead of counting them as
            // fresh successes.
            if (sg == dg) {
                connectFailures.push_back({
                    cc.srcId.toStdString(), cc.destId.toStdString(),
                    "self-connection (same object src==dest) — forbidden, breaks DSP scheduling" });
                continue;
            }
            if (cc.srcOut < 0 || cc.srcOut >= obj_noutlets(so)) {
                connectFailures.push_back({
                    cc.srcId.toStdString(), cc.destId.toStdString(),
                    "src outlet " + std::to_string(cc.srcOut) + " out of range (object has "
                        + std::to_string(obj_noutlets(so)) + ")" });
                continue;
            }
            if (cc.destIn < 0 || cc.destIn >= obj_ninlets(d_o)) {
                connectFailures.push_back({
                    cc.srcId.toStdString(), cc.destId.toStdString(),
                    "dest inlet " + std::to_string(cc.destIn) + " out of range (object has "
                        + std::to_string(obj_ninlets(d_o)) + ")" });
                continue;
            }
            // Duplicate-wire check: Pd silently accepts re-connecting
            // an existing wire; count it as a no-op, not a fresh success.
            // Walk the source outlet's connection list (Pd traverse API).
            bool alreadyWired = false;
            {
                t_outlet* srcOutlet = nullptr;
                t_outconnect* oc = obj_starttraverseoutlet(so, &srcOutlet, cc.srcOut);
                while (oc) {
                    t_object* destObj = nullptr;
                    t_inlet* destInl = nullptr;
                    int which = -1;
                    oc = obj_nexttraverseoutlet(oc, &destObj, &destInl, &which);
                    if (destObj == d_o && which == cc.destIn) {
                        alreadyWired = true;
                        break;
                    }
                }
            }
            if (alreadyWired) {
                connectFailures.push_back({
                    cc.srcId.toStdString(), cc.destId.toStdString(),
                    "duplicate wire (already connected) — no-op, not counted" });
                continue;
            }
            // R1 — Rate guard (PRD preflight, 2026-09-09): signal outlet
            // -> control inlet. Same rule as Interface::canConnect
            // (return !obj_issignaloutlet || obj_issignalinlet). The
            // GUI enforces it; batch_atomic previously bypassed it
            // via raw obj_connect. Now REJECT with stable prefix.
            {
                bool srcSig  = obj_issignaloutlet(so,  cc.srcOut) != 0;
                bool destSig = obj_issignalinlet(d_o, cc.destIn)  != 0;
                if (srcSig && !destSig) {
                    // Class-aware hint. The classic trap: [*~ 0.5] has a
                    // FLOAT inlet 1, so an envelope signal into it is
                    // correctly rejected — while a BARE [*~] has a SIGNAL
                    // inlet 1 and IS the canonical VCA. The old wording
                    // ("rewire via [*~]") left the model flailing, since
                    // the wire already pointed at [*~].
                    // ASCII only: non-ASCII in a reply string is mangled to
                    // mojibake on the C++ -> OSC -> TS path (see line 667).
                    juce::String hint = " - Pd drops the audio; use [snapshot~] to read it as control";
                    {
                        const char* dcls = class_getname(pd_class(&d_o->ob_pd));
                        juce::String dclsStr(dcls ? dcls : "");
                        if (dclsStr == "*~" || dclsStr == "+~" || dclsStr == "-~" || dclsStr == "/~") {
                            char* otext = nullptr; int olen = 0;
                            mcpGetObjectText(d_o, &otext, &olen);
                            juce::String objTxt = (otext && olen > 0)
                                ? juce::String::fromUTF8(otext, olen) : dclsStr;
                            if (otext && olen > 0) freebytes(otext, olen);
                            hint = " - [" + objTxt.trim()
                                 + "] has an ARGUMENT, so its inlet 1 is a FLOAT (control) inlet and a signal there is dropped. "
                                   "For an envelope VCA use a BARE [*~] (no args): its inlet 1 IS a signal inlet. "
                                   "Keep the level in a separate [*~ gain].";
                        }
                    }
                    connectFailures.push_back({
                        cc.srcId.toStdString(), cc.destId.toStdString(),
                        "rate: signal outlet " + std::to_string(cc.srcOut)
                          + " -> control inlet " + std::to_string(cc.destIn)
                          + hint.toStdString() });
                    continue;
                }
                // Guard 2: control outlet -> signal inlet. Pd accepts
                // this (sets the inlet's scalar, updated once per block)
                // so we do NOT reject — and it is NOT a required bridge.
                // Wording matches the TS rate-advisory (canonicalFix):
                // "legal in Pd (no bridge needed)".
                if (!srcSig && destSig) {
                    connectAdvisories.push_back({
                        cc.srcId.toStdString(), cc.destId.toStdString(),
                        "control outlet " + std::to_string(cc.srcOut)
                          + " -> signal inlet " + std::to_string(cc.destIn)
                          + " — legal in Pd (the float sets the signal inlet; no bridge needed). "
                            "Optional: [sig~] only if you want audio-rate smoothing" });
                }
            }
            // R2 — would this wire close a zero-delay signal loop?
            // Only signal-outlet wires can close signal cycles; control
            // edges are not tracked. Tentative graph grows in batch order
            // so the FIRST loop-closing wire is rejected.
            if (cycleGuardActive && obj_issignaloutlet(so, cc.srcOut)) {
                std::unordered_set<t_gobj*> visited;
                std::vector<t_gobj*> stack;
                stack.push_back(dg);
                visited.insert(dg);
                bool closesCycle = false;
                while (!stack.empty() && !closesCycle) {
                    t_gobj* cur = stack.back(); stack.pop_back();
                    if (cur == sg) { closesCycle = true; break; }
                    auto it = tentativeSigAdj.find(cur);
                    if (it == tentativeSigAdj.end()) continue;
                    for (t_gobj* nxt : it->second) {
                        if (visited.insert(nxt).second) stack.push_back(nxt);
                    }
                }
                if (closesCycle) {
                    connectFailures.push_back({
                        cc.srcId.toStdString(), cc.destId.toStdString(),
                        "cycle: would close zero-delay signal loop — break it with [delwrite~]/[delread~] or [send~]/[receive~]" });
                    continue;
                }
                tentativeSigAdj[sg].push_back(dg);
            }
            if (obj_connect(so, cc.srcOut, d_o, cc.destIn))
                connected++;
            else
                connectFailures.push_back({
                    cc.srcId.toStdString(), cc.destId.toStdString(),
                    "obj_connect failed" });
        } else {
            connectFailures.push_back({
                cc.srcId.toStdString(), cc.destId.toStdString(),
                "not a patchable object" });
        }
    }
}

void mcp::BatchJob::scanConnections(const std::string& canonStr)
{
    // Step D (moved, bug #5): post-mutation connection scan — runs
    // AFTER PHASE 5 connect so the receipt reflects the wires this
    // batch just created. PHASE 0 adoption already minted tempIds
    // for GUI objects; PHASE 4 registered new creates; deletes
    // were erased — every live wire endpoint resolves.
    {
        auto canvasStr = canonStr;
        auto& canvasMap = processor->getMcp()->identity.canvasMap(cnv);
        std::unordered_map<t_gobj*, std::string> ptrToTempId;
        for (auto& [tid, ptr] : canvasMap)
            if (ptr) ptrToTempId[ptr] = tid;

        t_linetraverser lt;
        linetraverser_start(&lt, cnv);
        t_outconnect* ltOc = nullptr;
        while ((ltOc = linetraverser_next_nosize(&lt))) {
            t_gobj* sg = &lt.tr_ob->ob_g;
            t_gobj* dg = &lt.tr_ob2->ob_g;
            auto sIt = ptrToTempId.find(sg);
            auto dIt = ptrToTempId.find(dg);
            if (sIt == ptrToTempId.end() || dIt == ptrToTempId.end()) continue;
            detectedConnections.push_back({
                sIt->second,
                static_cast<int>(lt.tr_outno),
                dIt->second,
                static_cast<int>(lt.tr_inno)
            });
        }
    }
}
