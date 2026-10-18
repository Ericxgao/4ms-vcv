# Surge Rack to native MetaModule conversion

The Rack plugin uses `SurgeXTRack:*` identifiers and assignable modulation.
The native plugin uses `SurgeXTNative:*` identifiers and fixed CV inputs.
Renaming module slugs alone would lose modulation, saved engine settings and
the meaning of mapped controls. This converter translates that whole contract
inside the existing Hub Save/Wi-Fi workflow.

## Review path

Start with `convert()` in [surge_native_export.cc](surge_native_export.cc): it
copies the patch, expands supported Unison graphs, builds a conversion plan,
and publishes the candidate only after success. An `Error` leaves the caller's
patch unchanged. The writer reports the failure; Save/Wi-Fi stop when the
writer returns an empty document.

Then read `EffectSpec` and `effect_spec()` for the declarative FX mappings,
`Plan::modulation()` for helper selection, the per-model dispatch for saved
state/selector conversion, and the final mapping/cable rewrite. Read
`expand_unison()` separately: graph cloning has different invariants from
ordinary parameter conversion.

The implementation commit adds an unused converter. The integration commit
wires it into the patch writer and guards both Save and Wi-Fi failure paths.
This ordering keeps error handling and activation together.

## Coordinates and units

Input `PatchData` parameters and mapping ranges are normalized to their
**original Rack ranges**. The converter validates that contract before
translating values. Original module IDs remain stable; helpers are appended.
`Ref` tracks a source parameter's destination and translated mapping range,
including knob banks and MIDI mappings. Module parameters, input jacks and
output jacks have separate ID spaces.

`EffectSpec` uses named initializers so these fields can be reviewed in place:

| Field | Meaning |
| --- | --- |
| `name`, `count` | Rack FX suffix and number of engine parameter slots |
| `poly_param`, `poly_default` | Native Poly control ID and missing-state default |
| `clock_param`, `clock_input` | Native Clock Format control and native clock jack; source Rack FX clock jack is 4; -1 means absent |
| `switches` | Native destinations of the Rack wrapper's extra switches, in source order |
| `integers`, `booleans` | Source engine slots needing discrete conversion/type validation |
| `gains` | Per-target ratio of Rack CV coefficient to native CV coefficient; empty means 0.5 |
| `extra` | Native parameter ID/value pairs fixed by the Rack wrapper |
| `outputs` | Number of native output jacks |
| `input_routes` | Additional source Rack input to native input pairs |
| `overridden` | Source engine slots overwritten by wrapper switches/gates; their modulation is inert |
| `parameter_ids` | Source engine slot to native parameter ID; empty means identity, -1 means unused |
| `cv_ids` | Source engine slot to native input ID; empty uses the module's default CV base, -1 means no direct CV destination |

The authoritative Rack definitions are `src/FX.h` and `src/fxconfig/*.h` in
the companion Surge repository. Native coefficients live in its
`metamodule/dsp/*_dsp.cpp`. Some gains are fractions because the Rack and
native ranges differ; they are not arbitrary output attenuation.

## Worked example: one input, two editable depths

Take a Chorus in Sum mode. Rack modulation input 1 controls Time at depth
`+0.5` and Feedback at `-0.25`; other depths are zero and unmapped. Its source
voltage is `v`. Both targets have conversion gain `g = 0.5`.

`Plan::modulation()` sees two active routes and a connected source, so it adds
one `4msCompany:Atvert2` helper. Both helper inputs receive the same source.
Its outputs connect only to native Chorus Time CV (input 2) and Feedback CV
(input 5). The source can therefore retain two independently editable depths.

The helper's normalized controls become:

| Route | Formula `0.5 + depth*g/2` | Parameter | Output |
| --- | --- | --- | --- |
| Time | `0.5 + 0.5*0.5/2` | 0.625 | `0.25*v` |
| Feedback | `0.5 - 0.25*0.5/2` | 0.4375 | `-0.125*v` |

Chorus's native ordinary CV coefficient is 0.2/V. Those outputs produce
normalized changes `+0.05*v` and `-0.025*v`, matching Rack's 0.1/V coefficient
times the original depths. A Rack knob/MIDI mapping to either depth is
translated to the corresponding helper control and range as well.

For a matrix route, the normalized depth is `0.5 + depth*g/4`, because the
matrix outputs `2*(2*p-1)*v`. The same routes therefore use 0.5625 and
0.46875. The resulting voltages are identical. A matrix is retained when the
compact form cannot preserve required behavior, such as multiple mixed-width
sources feeding a polyphonic destination, larger routing sets, or selector
conversion. Generic matrices have eight destination slots; unused destination
banks are not created, and unused outputs are not wired.

Nonzero depths and explicitly mapped depth controls both count as assigned,
even if a source is currently disconnected. Required selector outputs can
also force a route. The helper's layout does not shrink to match its cables;
named variants provide static destination labels and control groups.

## Compatibility and failure behavior

Saved state covers output filtering, table data and module-specific switches,
as well as markers that select Rack clock/initialization history in the native
processor. The markers must match native readers. Reverb's 0.1.45 marker is
required for the corresponding voice-history behavior; older exports need
re-exporting to acquire it.

Alias waveform and Unison count transformations happen after base and CV
summation. Transforming only the saved knob value would change selection
thresholds under modulation. Unison graph expansion preserves states, knob
banks and MIDI targets across four-lane copies, rejecting unsupported graphs
and higher expander banks instead of silently dropping voices.

Conversion fails for unsupported state, unrepresentable mappings, malformed
table payloads or exceeding the 64-module patch limit. Helpers add processing
and scheduling cost. Per-helper scheduling latency and RAM headroom remain
unmeasured; the native workload measurements below do not isolate those costs.
The Base64 implementation is duplicated in the native port's
`metamodule/dsp/table_encoding.hh`; format changes must update both copies.

## Existing evidence and limits

The combined 0.1.45 Hub build passed three export/expander/network suites,
Windows loading, and export/simulator coverage of all 46 Rack module types.
Those results include the separate HTTP-response and class-name fixes. The
isolated commits and current source organization have not been rebuilt.
The native companion's [PR description](https://github.com/Ericxgao/surge-rack/pull/1)
contains the validation summary and full hardware CPU table, including the
Spring optimization. That change preserves exported IDs, mappings and
compatibility markers; this converter remains compatible.
