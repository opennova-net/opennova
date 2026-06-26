# IDA Witness Task Template

## Goal

Answer original retail behavior or verify an OpenNova implementation against
retail.

## Preconditions

- Correct retail `Jointops.exe` IDB.
- Imagebase `0x400000`.
- Existing docs and correspondence checked.

## Steps

1. Choose `engine-research` for new behavior or `grill-ida` for parity review.
2. Anchor the function with xrefs, strings, constants, call graph, or captures.
3. Record confidence: anchored, probable, or guessed.
4. Compare the specific behavior axis needed for the task.
5. Update tracked docs through the RE-doc workflow when the witness changes
   project knowledge.

## Output

- `[orig: Name @ 0xADDR]` citations.
- Verdict and confidence.
- D-NET or correspondence update if needed.
- Tests or capture work needed next.

