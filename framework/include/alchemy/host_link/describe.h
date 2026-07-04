/**
 * @file alchemy/host_link/describe.h
 * @brief RenderDescriptor — derive the module's descriptor JSON from its
 *        managed components instead of hand-writing a builder table.
 *
 * Walks the Presets manage list in order (so descriptor order can never
 * drift from the blob layout) and describes each component by the first
 * source that claims it:
 *
 *   1. a DescribeOverride registered for that component,
 *   2. the built-in describers for the standard surfaces
 *      (Pager / Settings / PresetName, recognized via DescribeKind()),
 *   3. the component's own Serializable::Describe() override,
 *   4. an opaque fallback (byte-exact round-trip, generic label).
 *
 * Pager fields take their names, stable ids, and display hints from the
 * declared VirtualKnobs when a PageSet is supplied (names from Name(),
 * ids from Ident() or positional "p<page>.<pot>", hints derived from the
 * value transform / Unit() / Labels() / Disp()).  Page tabs come from
 * Page::Name()/Color().  Without a PageSet the pager still describes
 * fully — with positional ids and generic labels.
 *
 * All DescriptorBuilder drift guards apply; any failure returns 0 and
 * the module reports "no descriptor" (hosts fall back to raw backup).
 *
 * This layer is host-buildable and unit-tested; hostlink::Host wraps it
 * on target with transport/buffer/uid/reboot defaults.
 */

#pragma once

#include <cstddef>
#include <cstdint>
#include "alchemy/host_link/component_writer.h"
#include "alchemy/host_link/descriptor.h"

namespace alchemy {

class Page;
class Presets;
class Serializable;

namespace hostlink {

/** Per-component description override (fn-pointer + context; highest
 *  priority).  Return true after writing the component via @p w; a false
 *  return fails the whole build. */
struct DescribeOverride
{
    const Serializable* component;
    bool (*fn)(ComponentWriter& w, void* ctx);
    void* ctx;
};

/** Declared pages (knob names/ids/hints + tab metadata) for pager field
 *  derivation.  Entries may be null; page indices need not be dense. */
struct PageSet
{
    const Page* const* pages;
    uint8_t            count;
};

/**
 * Render the full descriptor into @p buf.  @p pages and @p overrides are
 * optional (pass nullptr / 0).  Returns the descriptor length, or 0 on
 * any drift, overflow, or describer failure.
 */
uint32_t RenderDescriptor(char* buf, size_t cap,
                          const DescriptorBuilder::ModuleInfo& info,
                          const Presets& presets,
                          const PageSet* pages,
                          const DescribeOverride* overrides,
                          uint8_t num_overrides);

} // namespace hostlink
} // namespace alchemy
