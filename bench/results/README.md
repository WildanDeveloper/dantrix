Results are stored here as a directory per run, named `<date>-<commit>/`,
written by `bench/measure.py --save`. Each directory holds:

| File | Contents |
|---|---|
| `results.md` | The measured tables, ready to paste into the README |
| `environment.txt` | Hardware, kernel, compiler, binary version, commit, server flags |
| `command.txt` | The exact command line, so the run can be repeated |

The README's benchmark principles require a result to be recorded together with
the environment it was measured in, and `bench/measure.py --save` is the code
that enforces it. A run that is not saved this way is not a record.

The directory is git-ignored: results describe one machine at one commit and
are quoted in the README, not committed as history.
