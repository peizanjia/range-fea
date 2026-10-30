Set-StrictMode -Version Latest

function Get-V43SaveField {
    param([AllowNull()][object]$InputObject, [Parameter(Mandatory = $true)][string]$Name)

    if ($null -eq $InputObject) { return $null }
    if ($InputObject -is [System.Collections.IDictionary]) {
        if ($InputObject.Contains($Name)) { return ,$InputObject[$Name] }
        return $null
    }
    $property = $InputObject.PSObject.Properties[$Name]
    if ($null -eq $property) { return $null }
    return ,$property.Value
}

function Test-V43SaveUtcTimestamp {
    param([AllowNull()][object]$Value, [ref][DateTimeOffset]$Parsed)

    if ($Value -isnot [string] -or $Value -notmatch 'Z$') { return $false }
    return [DateTimeOffset]::TryParse(
        $Value,
        [Globalization.CultureInfo]::InvariantCulture,
        ([Globalization.DateTimeStyles]::AssumeUniversal -bor [Globalization.DateTimeStyles]::AdjustToUniversal),
        $Parsed
    )
}

function New-V43SavePredicateDecision {
    param(
        [Parameter(Mandatory = $true)][string]$Decision,
        [Parameter(Mandatory = $true)][string]$ReasonCode,
        [string]$CandidateManifestIdentitySha256,
        [string]$AuthorityId,
        [string]$ObservationId,
        [string]$OperationId,
        [string[]]$MissingInputs = @(),
        [int]$PublicationRecordCount = 0,
        [int]$CloseRecordCount = 0
    )

    return [pscustomobject][ordered]@{
        schema = 'range.pc11-v43-save-predicate-decision/v1'
        decision = $Decision
        reasonCode = $ReasonCode
        candidateManifestIdentitySha256 = $CandidateManifestIdentitySha256
        authorityId = $AuthorityId
        observationId = $ObservationId
        operationId = $OperationId
        missingInputs = @($MissingInputs)
        publicationRecordCount = $PublicationRecordCount
        closeRecordCount = $CloseRecordCount
        fixtureOnlyDecision = $true
        productActionDecision = 'NO_ACTION'
        productActions = [ordered]@{
            guiLaunched = $false
            savePerformed = $false
            runPerformed = $false
            confirmPerformed = $false
        }
        goPermitted = $false
    }
}

function Get-V43SavePredicateDecision {
    [CmdletBinding()]
    param(
        [AllowNull()][object]$UiaEvidence,
        [AllowNull()][object]$OperationLog,
        [AllowNull()][object]$AuthorityEvidence,
        [Parameter(Mandatory = $true)][string]$EvaluationTimeUtc
    )

    $missingInputs = [System.Collections.Generic.List[string]]::new()
    if ($null -eq $UiaEvidence) { [void]$missingInputs.Add('uiaEvidence') }
    if ($null -eq $OperationLog) { [void]$missingInputs.Add('operationLog') }
    if ($null -eq $AuthorityEvidence) { [void]$missingInputs.Add('authorityEvidence') }
    if ($missingInputs.Count -gt 0) {
        return New-V43SavePredicateDecision -Decision 'EVIDENCE_INCOMPLETE' -ReasonCode 'MISSING_INPUT' -MissingInputs $missingInputs.ToArray()
    }

    $schemas = @(
        @($UiaEvidence, 'range.pc11-v43-save-uia-evidence/v1', 'UIA evidence'),
        @($OperationLog, 'range.pc11-v43-save-operation-log/v1', 'operation log'),
        @($AuthorityEvidence, 'range.pc11-v43-save-authority/v1', 'authority evidence')
    )
    foreach ($row in $schemas) {
        $schemaValue = Get-V43SaveField $row[0] 'schema'
        if ($schemaValue -isnot [string] -or $schemaValue -cne [string]$row[1]) {
            return New-V43SavePredicateDecision -Decision 'EVIDENCE_INVALID' -ReasonCode ('SCHEMA_MISMATCH_' + ([string]$row[2]).Replace(' ', '_').ToUpperInvariant())
        }
    }

    $authorityId = [string](Get-V43SaveField $AuthorityEvidence 'authorityId')
    $observationId = [string](Get-V43SaveField $UiaEvidence 'observationId')
    $candidateIdentity = [string](Get-V43SaveField $AuthorityEvidence 'candidateManifestIdentitySha256')
    $requiredStrings = @(
        @($AuthorityEvidence, 'authorityId'),
        @($AuthorityEvidence, 'candidateManifestIdentitySha256'),
        @($AuthorityEvidence, 'projectId'),
        @($AuthorityEvidence, 'documentId'),
        @($AuthorityEvidence, 'sourceRevision'),
        @($AuthorityEvidence, 'sourceSha256'),
        @($AuthorityEvidence, 'scope'),
        @($AuthorityEvidence, 'validFromUtc'),
        @($AuthorityEvidence, 'validUntilUtc'),
        @($UiaEvidence, 'candidateManifestIdentitySha256'),
        @($OperationLog, 'candidateManifestIdentitySha256'),
        @($UiaEvidence, 'observationId'),
        @($UiaEvidence, 'projectId'),
        @($UiaEvidence, 'documentId'),
        @($UiaEvidence, 'modelId'),
        @($UiaEvidence, 'sourceRevision'),
        @($UiaEvidence, 'modelSha256'),
        @($UiaEvidence, 'observedAtUtc'),
        @($UiaEvidence, 'documentState')
    )
    foreach ($required in $requiredStrings) {
        $value = Get-V43SaveField $required[0] ([string]$required[1])
        if ($value -isnot [string] -or [string]::IsNullOrWhiteSpace([string]$value)) {
            return New-V43SavePredicateDecision -Decision 'EVIDENCE_INCOMPLETE' -ReasonCode ('MISSING_FIELD_' + ([string]$required[1]).ToUpperInvariant()) -CandidateManifestIdentitySha256 $candidateIdentity -AuthorityId $authorityId -ObservationId $observationId
        }
    }

    if ($candidateIdentity -notmatch '^[0-9a-fA-F]{64}$') {
        return New-V43SavePredicateDecision -Decision 'EVIDENCE_INVALID' -ReasonCode 'MALFORMED_CANDIDATE_IDENTITY' -AuthorityId $authorityId -ObservationId $observationId
    }
    foreach ($shaField in @(@($AuthorityEvidence, 'sourceSha256'), @($UiaEvidence, 'modelSha256'))) {
        if ([string](Get-V43SaveField $shaField[0] ([string]$shaField[1])) -notmatch '^[0-9a-fA-F]{64}$') {
            return New-V43SavePredicateDecision -Decision 'EVIDENCE_INVALID' -ReasonCode 'MALFORMED_SOURCE_SHA256' -CandidateManifestIdentitySha256 $candidateIdentity -AuthorityId $authorityId -ObservationId $observationId
        }
    }
    $scope = Get-V43SaveField $AuthorityEvidence 'scope'
    if ($scope -isnot [string] -or $scope -cne 'save-evidence-review') {
        return New-V43SavePredicateDecision -Decision 'EVIDENCE_INVALID' -ReasonCode 'AUTHORITY_SCOPE_MISMATCH' -CandidateManifestIdentitySha256 $candidateIdentity -AuthorityId $authorityId -ObservationId $observationId
    }

    $evaluationTime = [DateTimeOffset]::MinValue
    $validFrom = [DateTimeOffset]::MinValue
    $validUntil = [DateTimeOffset]::MinValue
    $uiaTime = [DateTimeOffset]::MinValue
    if (-not (Test-V43SaveUtcTimestamp $EvaluationTimeUtc ([ref]$evaluationTime)) -or
        -not (Test-V43SaveUtcTimestamp (Get-V43SaveField $AuthorityEvidence 'validFromUtc') ([ref]$validFrom)) -or
        -not (Test-V43SaveUtcTimestamp (Get-V43SaveField $AuthorityEvidence 'validUntilUtc') ([ref]$validUntil)) -or
        -not (Test-V43SaveUtcTimestamp (Get-V43SaveField $UiaEvidence 'observedAtUtc') ([ref]$uiaTime))) {
        return New-V43SavePredicateDecision -Decision 'EVIDENCE_INVALID' -ReasonCode 'MALFORMED_UTC_TIMESTAMP' -CandidateManifestIdentitySha256 $candidateIdentity -AuthorityId $authorityId -ObservationId $observationId
    }
    if ($validUntil -lt $validFrom -or $evaluationTime -lt $validFrom -or $evaluationTime -gt $validUntil -or $uiaTime -lt $validFrom -or $uiaTime -gt $validUntil) {
        return New-V43SavePredicateDecision -Decision 'EVIDENCE_STALE' -ReasonCode 'AUTHORITY_OUTSIDE_VALIDITY_WINDOW' -CandidateManifestIdentitySha256 $candidateIdentity -AuthorityId $authorityId -ObservationId $observationId
    }
    if ($uiaTime -gt $evaluationTime) {
        return New-V43SavePredicateDecision -Decision 'EVIDENCE_STALE' -ReasonCode 'UIA_OBSERVATION_FROM_FUTURE' -CandidateManifestIdentitySha256 $candidateIdentity -AuthorityId $authorityId -ObservationId $observationId
    }

    $identityFields = @(
        @($UiaEvidence, 'candidateManifestIdentitySha256'),
        @($OperationLog, 'candidateManifestIdentitySha256')
    )
    foreach ($identityField in $identityFields) {
        if ([string](Get-V43SaveField $identityField[0] ([string]$identityField[1])) -ine $candidateIdentity) {
            return New-V43SavePredicateDecision -Decision 'EVIDENCE_STALE' -ReasonCode 'CANDIDATE_PROVENANCE_MISMATCH' -CandidateManifestIdentitySha256 $candidateIdentity -AuthorityId $authorityId -ObservationId $observationId
        }
    }

    foreach ($field in @('projectId', 'documentId', 'sourceRevision')) {
        if ([string](Get-V43SaveField $UiaEvidence $field) -cne [string](Get-V43SaveField $AuthorityEvidence $field)) {
            return New-V43SavePredicateDecision -Decision 'EVIDENCE_STALE' -ReasonCode ('UIA_AUTHORITY_' + $field.ToUpperInvariant() + '_MISMATCH') -CandidateManifestIdentitySha256 $candidateIdentity -AuthorityId $authorityId -ObservationId $observationId
        }
    }
    if ([string](Get-V43SaveField $UiaEvidence 'modelSha256') -ine [string](Get-V43SaveField $AuthorityEvidence 'sourceSha256')) {
        return New-V43SavePredicateDecision -Decision 'EVIDENCE_STALE' -ReasonCode 'UIA_AUTHORITY_SOURCE_SHA256_MISMATCH' -CandidateManifestIdentitySha256 $candidateIdentity -AuthorityId $authorityId -ObservationId $observationId
    }
    $dirtyField = Get-V43SaveField $UiaEvidence 'dirty'
    if ($dirtyField -isnot [bool]) {
        return New-V43SavePredicateDecision -Decision 'EVIDENCE_INVALID' -ReasonCode 'UIA_DIRTY_FLAG_NOT_BOOLEAN' -CandidateManifestIdentitySha256 $candidateIdentity -AuthorityId $authorityId -ObservationId $observationId
    }
    if ((Get-V43SaveField $UiaEvidence 'documentState') -cne 'SAVED' -or $dirtyField) {
        return New-V43SavePredicateDecision -Decision 'EVIDENCE_INCOMPLETE' -ReasonCode 'UIA_DOCUMENT_NOT_QUIESCENT' -CandidateManifestIdentitySha256 $candidateIdentity -AuthorityId $authorityId -ObservationId $observationId
    }

    $records = Get-V43SaveField $OperationLog 'records'
    if ($records -isnot [array]) {
        return New-V43SavePredicateDecision -Decision 'EVIDENCE_INCOMPLETE' -ReasonCode 'MISSING_OPERATION_RECORDS' -CandidateManifestIdentitySha256 $candidateIdentity -AuthorityId $authorityId -ObservationId $observationId
    }

    $publications = [System.Collections.Generic.List[object]]::new()
    $closes = [System.Collections.Generic.List[object]]::new()
    foreach ($record in $records) {
        $eventType = Get-V43SaveField $record 'eventType'
        if ($eventType -isnot [string]) { continue }
        if ($eventType -cnotin @('artifact.published', 'session.closed')) { continue }
        $recordProjectId = Get-V43SaveField $record 'projectId'
        $recordDocumentId = Get-V43SaveField $record 'documentId'
        if ($recordProjectId -isnot [string] -or $recordDocumentId -isnot [string] -or
            $recordProjectId -cne [string](Get-V43SaveField $AuthorityEvidence 'projectId') -or
            $recordDocumentId -cne [string](Get-V43SaveField $AuthorityEvidence 'documentId')) { continue }
        if ($eventType -ceq 'artifact.published') { [void]$publications.Add($record) }
        else { [void]$closes.Add($record) }
    }

    if ($publications.Count -gt 1 -or $closes.Count -gt 1) {
        return New-V43SavePredicateDecision -Decision 'EVIDENCE_AMBIGUOUS' -ReasonCode 'DUPLICATE_PROVENANCE_RECORDS' -CandidateManifestIdentitySha256 $candidateIdentity -AuthorityId $authorityId -ObservationId $observationId -PublicationRecordCount $publications.Count -CloseRecordCount $closes.Count
    }
    if ($publications.Count -eq 0 -or $closes.Count -eq 0) {
        return New-V43SavePredicateDecision -Decision 'EVIDENCE_INCOMPLETE' -ReasonCode 'PARTIAL_OPERATION_PROVENANCE' -CandidateManifestIdentitySha256 $candidateIdentity -AuthorityId $authorityId -ObservationId $observationId -PublicationRecordCount $publications.Count -CloseRecordCount $closes.Count
    }

    $publication = $publications[0]
    $close = $closes[0]
    $operationId = [string](Get-V43SaveField $publication 'operationId')
    $requiredRecordFields = @('eventId', 'operationId', 'candidateManifestIdentitySha256', 'modelId', 'sourceRevision', 'sourceSha256', 'occurredAtUtc')
    foreach ($record in @($publication, $close)) {
        foreach ($field in $requiredRecordFields) {
            $value = Get-V43SaveField $record $field
            if ($value -isnot [string] -or [string]::IsNullOrWhiteSpace([string]$value)) {
                return New-V43SavePredicateDecision -Decision 'EVIDENCE_INCOMPLETE' -ReasonCode ('MISSING_RECORD_FIELD_' + $field.ToUpperInvariant()) -CandidateManifestIdentitySha256 $candidateIdentity -AuthorityId $authorityId -ObservationId $observationId -OperationId $operationId -PublicationRecordCount 1 -CloseRecordCount 1
            }
        }
    }
    if ([string](Get-V43SaveField $publication 'eventId') -ceq [string](Get-V43SaveField $close 'eventId')) {
        return New-V43SavePredicateDecision -Decision 'EVIDENCE_AMBIGUOUS' -ReasonCode 'DUPLICATE_PROVENANCE_RECORD_ID' -CandidateManifestIdentitySha256 $candidateIdentity -AuthorityId $authorityId -ObservationId $observationId -OperationId $operationId -PublicationRecordCount 1 -CloseRecordCount 1
    }
    if ([string](Get-V43SaveField $publication 'operationId') -cne [string](Get-V43SaveField $close 'operationId') -or
        [string](Get-V43SaveField $publication 'modelId') -cne [string](Get-V43SaveField $close 'modelId')) {
        return New-V43SavePredicateDecision -Decision 'EVIDENCE_STALE' -ReasonCode 'OPERATION_PROVENANCE_MISMATCH' -CandidateManifestIdentitySha256 $candidateIdentity -AuthorityId $authorityId -ObservationId $observationId -OperationId $operationId -PublicationRecordCount 1 -CloseRecordCount 1
    }

    foreach ($record in @($publication, $close)) {
        if ([string](Get-V43SaveField $record 'candidateManifestIdentitySha256') -ine $candidateIdentity -or
            [string](Get-V43SaveField $record 'sourceRevision') -cne [string](Get-V43SaveField $AuthorityEvidence 'sourceRevision') -or
            [string](Get-V43SaveField $record 'sourceSha256') -ine [string](Get-V43SaveField $AuthorityEvidence 'sourceSha256')) {
            return New-V43SavePredicateDecision -Decision 'EVIDENCE_STALE' -ReasonCode 'OPERATION_SOURCE_PROVENANCE_MISMATCH' -CandidateManifestIdentitySha256 $candidateIdentity -AuthorityId $authorityId -ObservationId $observationId -OperationId $operationId -PublicationRecordCount 1 -CloseRecordCount 1
        }
    }
    foreach ($record in @($publication, $close)) {
        if ([string](Get-V43SaveField $record 'modelId') -cne [string](Get-V43SaveField $UiaEvidence 'modelId')) {
            return New-V43SavePredicateDecision -Decision 'EVIDENCE_STALE' -ReasonCode 'UIA_OPERATION_MODEL_MISMATCH' -CandidateManifestIdentitySha256 $candidateIdentity -AuthorityId $authorityId -ObservationId $observationId -OperationId $operationId -PublicationRecordCount 1 -CloseRecordCount 1
        }
        $recordTime = [DateTimeOffset]::MinValue
        if (-not (Test-V43SaveUtcTimestamp (Get-V43SaveField $record 'occurredAtUtc') ([ref]$recordTime))) {
            return New-V43SavePredicateDecision -Decision 'EVIDENCE_INVALID' -ReasonCode 'MALFORMED_OPERATION_UTC_TIMESTAMP' -CandidateManifestIdentitySha256 $candidateIdentity -AuthorityId $authorityId -ObservationId $observationId -OperationId $operationId -PublicationRecordCount 1 -CloseRecordCount 1
        }
        if ($recordTime -lt $validFrom -or $recordTime -gt $validUntil -or $recordTime -gt $evaluationTime) {
            return New-V43SavePredicateDecision -Decision 'EVIDENCE_STALE' -ReasonCode 'OPERATION_OUTSIDE_VALIDITY_WINDOW' -CandidateManifestIdentitySha256 $candidateIdentity -AuthorityId $authorityId -ObservationId $observationId -OperationId $operationId -PublicationRecordCount 1 -CloseRecordCount 1
        }
    }
    $publicationTime = [DateTimeOffset]::MinValue
    $closeTime = [DateTimeOffset]::MinValue
    [void](Test-V43SaveUtcTimestamp (Get-V43SaveField $publication 'occurredAtUtc') ([ref]$publicationTime))
    [void](Test-V43SaveUtcTimestamp (Get-V43SaveField $close 'occurredAtUtc') ([ref]$closeTime))
    if ($closeTime -lt $publicationTime) {
        return New-V43SavePredicateDecision -Decision 'EVIDENCE_STALE' -ReasonCode 'OPERATION_EVENT_ORDER_INVALID' -CandidateManifestIdentitySha256 $candidateIdentity -AuthorityId $authorityId -ObservationId $observationId -OperationId $operationId -PublicationRecordCount 1 -CloseRecordCount 1
    }

    return New-V43SavePredicateDecision -Decision 'EVIDENCE_COMPLETE' -ReasonCode 'UNIQUE_CURRENT_AUTHORITY_UIA_AND_OPERATION_CHAIN' -CandidateManifestIdentitySha256 $candidateIdentity.ToLowerInvariant() -AuthorityId $authorityId -ObservationId $observationId -OperationId $operationId -PublicationRecordCount 1 -CloseRecordCount 1
}
