# Recording

Recording lives in the machines now; each one records the stereo line input
in its own way. The details are on each machine's page.

| Where | What it records | Lands as |
|---|---|---|
| [Sampler](docs/machines/sampler.md) | Arm a voice, then a gate records into it | `usr/REC/REC_NNNN` |
| [Tape](docs/machines/tape.md) | TR2 punches a take into the tape (or straight to the card with Rec Dest = card) | `usr/TAPE/CUT_NNNN` / `TLP_NNNN`; card takes `usr/REC/TAP_NNNN` |
| [Looper](docs/machines/looper.md) | Four RAM tracks, clock-synced; **Save Trk** writes one out | `usr/LOOPS/LOOPNNNN` |
| Bounce (any machine) | The machine's *output*, from the web BOUNCE card or `POST /bounce/start` / `stop` | `usr/REC/BNC_NNNN` |

Every recording is a normal pool sample: it shows up in each machine's
browser (newest first under `ALL/`) and in the web Files card, and can be
renamed, moved or loaded into another machine from there.

The panel's **GAIN** knob sets the input level. There is no mic input and no
CV/gate output.
