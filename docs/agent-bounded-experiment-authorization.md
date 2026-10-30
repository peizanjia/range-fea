# Bounded experiment authorization

The Assistant's **Authorize experiment…** control accepts a JSON file containing an explicit input ProblemSpec and numerical policy. This is a trusted user/host input, never a model tool. Obtain the current document ID, session epoch and revision from the Host context first. The file has this structure:

```json
{
  "target": {"documentId": "CURRENT_DOCUMENT_ID", "sessionEpoch": "CURRENT_SESSION_EPOCH"},
  "expectedDocumentRevision": 1,
  "arguments": {
    "problemSpec": {"schemaVersion": 1, "modelType": "..."},
    "budgets": {"solves": 8, "repairs": 3, "modelTurns": 12},
    "allowedNumericalRanges": [],
    "stopConditions": {"requirePassingVerification": true, "stopOnSourceChange": true, "stopOnUnclassifiedFailure": true}
  }
}
```

The abbreviated `problemSpec` above is illustrative, not a valid analysis input. Supply the complete Host input schema with quantities and units. A bounded `convergenceStudy` may also be included under `arguments` with matching ranges. Host validation decides whether the input is valid.

The control requests a Host preview and displays the preview/experiment IDs, authorization digest, complete normalized SI problem, numerical policy, source binding and Host diagnostics in a scrollable review. Diagnostics must have the expected operation/schema and match the preview's source document and revision. Every issue must include a non-empty code and explanation plus an evidence object; missing or malformed diagnostics fail closed. An `error` diagnostic blocks authorization; a `warning` requires the reviewer to check an acknowledgement box before the dialog can continue. Confirming arms exactly one authorization for the next task; it is kept in memory, is discarded on task termination, conversation reset or change, and does not enable the general Auto checkbox. The selected live document, revision, epoch, source snapshot, normalized problem and all normalized numerical policies must match the model's later Host preview. A mismatch does not receive automatic approval. A matching preview is consumed once and still goes through the existing Host preview approval, durable execution intent and authorization checks. Execution facts record the mode and preauthorization preview, experiment and digest alongside the submitted preview identity. No source document mutation is authorized by this facility.

Input files can contain engineering information; treat them as project data. Do not place credentials in the file. Existing Auto behavior for other actions remains separately controlled. An armed authorization is not persisted for later sessions.
