/*
 // Copyright (c) 2026 PlugData MCP Team
 // For information on usage and redistribution, and for a DISCLAIMER OF ALL
 // WARRANTIES, see the file, "LICENSE.txt," in this distribution.
*/

// Bridge commands: arrays (Plan 3 P3.3): handleArrayDomain moved unchanged out of MCPBridge.cpp.

#include "MCPBridgeCmdCommon.h"

static t_garray* findGArrayInCanvas(t_canvas* cnv, t_symbol* nameSym)
{
    if (!cnv) return nullptr;
    for (t_gobj* y = cnv->gl_list; y; y = y->g_next) {
        if (pd_class(&y->g_pd) == garray_class) {
            t_garray* ga = reinterpret_cast<t_garray*>(y);
            t_symbol* sym = nullptr;
            if (garray_getname(ga, &sym) && sym == nameSym) {
                return ga;
            }
        } else if (pd_class(&y->g_pd) == canvas_class) {
            t_canvas* sub = reinterpret_cast<t_canvas*>(y);
            t_garray* ga = findGArrayInCanvas(sub, nameSym);
            if (ga) return ga;
        }
    }
    return nullptr;
}

void MCPBridge::handleArrayDomain(const juce::String& arrayAction, const juce::OSCMessage& msg)
{
    if (!processor || msg.size() < 2) return;

    // Two conventions:
    //   write/read: /array/<action> <name> <subpatch> <corrId> ...  (name = msg[0])
    //   stats:      /array ["stats", name, sampleRate, corrId]      (name = msg[1])
    auto arrayName = arrayAction == "stats" ? getArgString(msg[1]) : getArgString(msg[0]);
    auto canvasName = normalizeCanvas(getArgString(msg[1]));

    // /array/load <name> <subpatch> <filePath> <corrId>
    // Loads a WAV/AIFF from disk into the array natively (JUCE AudioFormatReader).
    // One message, no [soundfiler] object, no bang-and-wait dance. The disk read
    // happens OUTSIDE sys_lock; only resize+fill run under sys_lock. Reply arg0 = size,
    // or a negative error code.
    if (arrayAction == "load") {
        auto filePath      = msg.size() > 2 ? getArgString(msg[2]) : "";
        auto correlationId = msg.size() > 3 ? getArgString(msg[3]) : "0";
        juce::String replyAddr = "/array/load/reply/" + correlationId;

        juce::File file(filePath);
        if (!file.existsAsFile()) { sendReply(replyAddr, -2.0f); return; }

        juce::AudioFormatManager formatManager;
        formatManager.registerBasicFormats();
        std::unique_ptr<juce::AudioFormatReader> reader(formatManager.createReaderFor(file));
        if (!reader) { sendReply(replyAddr, -3.0f); return; }

        int const numChannels = static_cast<int>(reader->numChannels);
        int const numSamples  = static_cast<int>(reader->lengthInSamples);
        if (numChannels <= 0 || numSamples <= 0) { sendReply(replyAddr, -4.0f); return; }

        // Mono downmix (JUCE downmixes stereo→mono when the destination is 1ch).
        juce::AudioBuffer<float> fileBuf(1, numSamples);
        if (!reader->read(&fileBuf, 0, numSamples, 0, true, true)) { sendReply(replyAddr, -5.0f); return; }
        const float* src = fileBuf.getReadPointer(0);

        sys_lock();
        auto nameSym = gensym(arrayName.toRawUTF8());
        t_garray* garray = reinterpret_cast<t_garray*>(pd_findbyclass(nameSym, garray_class));
        if (!garray) {
            for (t_canvas* c = pd_this->pd_canvaslist; c; c = c->gl_next) {
                garray = findGArrayInCanvas(c, nameSym);
                if (garray) break;
            }
        }

        // Auto-create the array if it doesn't exist yet.
        if (!garray) {
            t_canvas* cnv = processor->getMcp()->getCanvasBySymbol(canvasName);
            if (!cnv && canvasName == "pd-main") cnv = pd_this->pd_canvaslist;
            if (cnv) {
                int nobj = 0;
                for (t_gobj* y = cnv->gl_list; y; y = y->g_next) nobj++;
                int stagger = (nobj * 14) % 420;
                juce::String pasta = "#N canvas 0 0 450 300 (subpatch) 0;\n#X array "
                    + arrayName + " " + juce::String(numSamples) + " float 2;\n#X coords 0 1 "
                    + juce::String(numSamples > 1 ? numSamples - 1 : 1) + " -1 200 140 1 0 0;\n#X restore "
                    + juce::String(60 + stagger) + " " + juce::String(60 + stagger) + " graph;";
                pd::Interface::paste(cnv, pasta.toRawUTF8());
                garray = reinterpret_cast<t_garray*>(pd_findbyclass(nameSym, garray_class));
                if (!garray) {
                    for (t_canvas* c = pd_this->pd_canvaslist; c; c = c->gl_next) {
                        garray = findGArrayInCanvas(c, nameSym);
                        if (garray) break;
                    }
                }
                if (garray) {
                    canvas_dirty(cnv, 1);
                    processor->getMcp()->enqueuePd([p = processor] { p->getMcp()->synchroniseCanvases(); });
                }
            }
        }

        if (!garray) { sys_unlock(); sendReply(replyAddr, -6.0f); return; }

        garray_resize_long(garray, numSamples);
        int size = 0;
        t_word* vec = nullptr;
        if (!garray_getfloatwords(garray, &size, &vec) || !vec) {
            sys_unlock();
            sendReply(replyAddr, -7.0f);
            return;
        }

        int const copy = size < numSamples ? size : numSamples;
        for (int i = 0; i < copy; ++i) vec[i].w_float = src[i];
        garray_redraw(garray);
        sys_unlock();

        sendReply(replyAddr, static_cast<float>(copy));
        return;
    }

    sys_lock();
    auto nameSym = gensym(arrayName.toRawUTF8());
    t_garray* garray = reinterpret_cast<t_garray*>(pd_findbyclass(nameSym, garray_class));
    if (!garray) {
        for (t_canvas* c = pd_this->pd_canvaslist; c; c = c->gl_next) {
            garray = findGArrayInCanvas(c, nameSym);
            if (garray) break;
        }
    }

    // PlugData instantiates arrays through the GUI; arrays created via the
    // MCP text-object path may never bind their garray. If a write targets a
    // missing array, create it as a graph-on-parent via paste (the same
    // mechanism pd::Patch::createObject uses for arrays).
    if (!garray && (arrayAction == "write" || arrayAction == "write_bulk") && msg.size() >= 4) {
        int reqSize = 2048;
        if (arrayAction == "write" && msg.size() >= 5) {
            int chunkIndex = static_cast<int>(getArgFloat(msg[3]));
            int dataCount = msg.size() - 5;
            reqSize = std::max(2048, chunkIndex * 128 + dataCount);
        } else if (arrayAction == "write_bulk" && msg.size() >= 4) {
            int offset = static_cast<int>(getArgFloat(msg[3]));
            int dataCount = msg.size() - 4;
            // A FULL write (offset 0) into a missing array is sized to the DATA so a
            // 2^n+3 wavetable stays tabosc4~-valid (the fixed 2048 floor produced an
            // invalid 2048-point table). The floor only remains a size ESTIMATE for a
            // PARTIAL write (offset > 0) into an array that does not exist yet.
            reqSize = offset > 0 ? std::max(2048, offset + dataCount) : std::max(1, dataCount);
        }

        t_canvas* cnv = processor->getMcp()->getCanvasBySymbol(canvasName);
        if (!cnv && canvasName == "pd-main") cnv = pd_this->pd_canvaslist;
        if (cnv) {
            // Stagger each auto-created array so they don't stack on top of
            // each other in the top-left corner.
            int nobj = 0;
            for (t_gobj* y = cnv->gl_list; y; y = y->g_next) nobj++;
            int stagger = (nobj * 14) % 420;
            juce::String pasta = "#N canvas 0 0 450 300 (subpatch) 0;\n#X array "
                + arrayName + " " + juce::String(reqSize) + " float 2;\n#X coords 0 1 "
                + juce::String(reqSize > 1 ? reqSize - 1 : 1) + " -1 200 140 1 0 0;\n#X restore "
                + juce::String(60 + stagger) + " " + juce::String(60 + stagger) + " graph;";
            pd::Interface::paste(cnv, pasta.toRawUTF8());
            garray = reinterpret_cast<t_garray*>(pd_findbyclass(nameSym, garray_class));
            if (!garray) {
                for (t_canvas* c = pd_this->pd_canvaslist; c; c = c->gl_next) {
                    garray = findGArrayInCanvas(c, nameSym);
                    if (garray) break;
                }
            }
            if (garray) {
                canvas_dirty(cnv, 1);
                processor->getMcp()->enqueuePd([p = processor] { p->getMcp()->synchroniseCanvases(); });
            }
        }
    }

    if (!garray) {
        sys_unlock();
        return;
    }

    int size = 0;
    t_word* vec = nullptr;
    if (!garray_getfloatwords(garray, &size, &vec) || !vec) {
        sys_unlock();
        return;
    }

    if (arrayAction == "write_bulk" && msg.size() >= 4) {
        // [name, subpatch, corrId, offset, data...]
        auto correlationId = getArgString(msg[2]);
        int offset = static_cast<int>(getArgFloat(msg[3]));
        int dataStart = 4;
        int dataCount = msg.size() - dataStart;

        int requiredSize = offset + dataCount;
        if (requiredSize > size) {
            garray_resize_long(garray, requiredSize);
            if (!garray_getfloatwords(garray, &size, &vec) || !vec) {
                sys_unlock();
                juce::OSCMessage errReply { juce::OSCAddressPattern("/array/write_bulk/reply/" + correlationId) };
                errReply.addArgument(static_cast<int32>(-1));
                errReply.addArgument(static_cast<int32>(0));
                sender.send(errReply);
                return;
            }
        }

        int written = 0;
        for (int i = dataStart; i < msg.size() && (offset + written) < size; ++i) {
            vec[offset + written].w_float = getArgFloat(msg[i]);
            written++;
        }
        garray_redraw(garray);
        sys_unlock();

        juce::OSCMessage reply { juce::OSCAddressPattern("/array/write_bulk/reply/" + correlationId) };
        reply.addArgument(static_cast<int32>(written));
        reply.addArgument(static_cast<int32>(size));
        sender.send(reply);
        return;
    } else if (arrayAction == "read_bulk") {
        // Format: [arrayName, subpatch, correlationId, offset, limit]
        auto correlationId = msg.size() > 2 ? getArgString(msg[2]) : "0";
        int offset = msg.size() > 3 ? static_cast<int>(getArgFloat(msg[3])) : 0;
        int limit = msg.size() > 4 ? static_cast<int>(getArgFloat(msg[4])) : size;

        int readStart = std::max(0, std::min(offset, size - 1));
        int readCount = std::min(limit, size - readStart);

        // Cap at 8192 samples per single reply (32KB OSC payload)
        readCount = std::min(readCount, 8192);

        juce::OSCMessage reply { juce::OSCAddressPattern("/array/read_bulk/reply/" + correlationId) };
        reply.addArgument(arrayName);
        reply.addArgument(static_cast<int32>(size));         // total array size
        reply.addArgument(static_cast<int32>(readStart));    // actual start
        reply.addArgument(static_cast<int32>(readCount));    // actual count
        for (int i = readStart; i < readStart + readCount; ++i) {
            reply.addArgument(vec[i].w_float);
        }
        sys_unlock();
        sender.send(reply);
        return;
    } else if (arrayAction == "write" && msg.size() >= 5) {
        // [name, subpatch, corrId, chunkIndex, totalChunks, data...]
        auto correlationId = getArgString(msg[2]);
        int chunkIndex = static_cast<int>(getArgFloat(msg[3]));
        int const CHUNK = 128;
        int offset = chunkIndex * CHUNK;
        int dataStart = 5;

        for (int i = dataStart; i < msg.size() && (offset + i - dataStart) < size; ++i) {
            vec[offset + i - dataStart].w_float = getArgFloat(msg[i]);
        }
        garray_redraw(garray);
        sys_unlock();

        sendReply("/array/write/reply/" + correlationId, static_cast<float>(size));
    } else if (arrayAction == "read") {
        auto correlationId = msg.size() > 2 ? getArgString(msg[2]) : "0";
        int offset = msg.size() > 3 ? static_cast<int>(getArgFloat(msg[3])) : 0;
        int limit = msg.size() > 4 ? static_cast<int>(getArgFloat(msg[4])) : 512;

        int readStart = std::max(0, std::min(offset, size - 1));
        int readCount = std::min(limit, size - readStart);

        int const CHUNK = 4096;
        int totalChunks = (readCount + CHUNK - 1) / CHUNK;

        for (int c = 0; c < totalChunks; ++c) {
            int start = readStart + c * CHUNK;
            int stop = std::min(start + CHUNK, readStart + readCount);

            juce::OSCMessage chunkMsg { juce::OSCAddressPattern("/array/read/chunk/" + correlationId) };
            chunkMsg.addArgument(static_cast<int32>(c));
            chunkMsg.addArgument(static_cast<int32>(totalChunks));
            for (int i = start; i < stop; ++i) {
                chunkMsg.addArgument(vec[i].w_float);
            }
            sender.send(chunkMsg);
        }
        sys_unlock();

        juce::OSCMessage doneMsg { juce::OSCAddressPattern("/array/read/done/" + correlationId) };
        doneMsg.addArgument(arrayName);
        doneMsg.addArgument(static_cast<int32>(size));
        sender.send(doneMsg);
    } else if (arrayAction == "stats") {
        float sampleRate = msg.size() > 2 ? getArgFloat(msg[2]) : 44100.0f;
        auto correlationId = msg.size() > 3 ? getArgString(msg[3]) : "0";

        float minVal = 0.0f, maxVal = 0.0f, sumSq = 0.0f;
        int zeroCrossings = 0;
        float prevVal = 0.0f;

        if (size > 0) {
            minVal = vec[0].w_float;
            maxVal = vec[0].w_float;
            for (int i = 0; i < size; ++i) {
                float v = vec[i].w_float;
                if (v < minVal) minVal = v;
                if (v > maxVal) maxVal = v;
                sumSq += v * v;

                if (i > 0) {
                    if ((prevVal < 0 && v >= 0) || (prevVal > 0 && v <= 0)) {
                        zeroCrossings++;
                    }
                }
                prevVal = v;
            }
        }
        sys_unlock();

        float rms = size > 0 ? std::sqrt(sumSq / static_cast<float>(size)) : 0.0f;
        float estimatedPitch = size > 0 ? (static_cast<float>(zeroCrossings) / 2.0f) * (sampleRate / static_cast<float>(size)) : 0.0f;

        juce::OSCMessage reply { juce::OSCAddressPattern("/array/stats/reply/" + correlationId) };
        reply.addArgument(arrayName);
        reply.addArgument(static_cast<int32>(size));
        reply.addArgument(minVal);
        reply.addArgument(maxVal);
        reply.addArgument(rms);
        reply.addArgument(static_cast<int32>(zeroCrossings));
        reply.addArgument(estimatedPitch);
        sender.send(reply);
    } else if (arrayAction == "mutate") {
        // /array/mutate <name> <subpatch> <corrId> <reverse|invert|crush>
        // In-place array mutation under a single sys_lock — zero OSC data round-trip,
        // so it never floods the bridge queue (the cause of live audio drops when
        // the TS server shuttles 427k floats through ~150 chunked messages).
        auto correlationId = msg.size() > 2 ? getArgString(msg[2]) : "0";
        auto op = msg.size() > 3 ? getArgString(msg[3]) : "reverse";

        if (op == "reverse") {
            for (int i = 0; i < size / 2; ++i) {
                float tmp = vec[i].w_float;
                vec[i].w_float = vec[size - 1 - i].w_float;
                vec[size - 1 - i].w_float = tmp;
            }
        } else if (op == "invert") {
            for (int i = 0; i < size; ++i) vec[i].w_float = -vec[i].w_float;
        } else if (op == "crush") {
            int const bits = 8;
            float const q = 1.0f / static_cast<float>(1 << (bits - 1));
            for (int i = 0; i < size; ++i) {
                float v = vec[i].w_float;
                vec[i].w_float = std::round(v / q) * q;
            }
        }

        garray_redraw(garray);
        sys_unlock();
        sendReply("/array/mutate/reply/" + correlationId, static_cast<float>(size));
        return;
    } else {
        sys_unlock();
    }
}
