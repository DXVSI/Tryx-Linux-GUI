pragma ComponentBehavior: Bound

import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

import "../components"

ScrollView {
    id: root

    required property var runtime
    required property var editor
    required property var deviceMedia

    clip: true

    property bool splitMode: false
    property var selectedMedia: []
    property var fullMetrics: []
    property var leftMetrics: []
    property var rightMetrics: []
    property var fullBadges: []
    property var leftBadges: []
    property var rightBadges: []
    property var fullBadgeChoices: emptyBadgeArea()
    property var leftBadgeChoices: emptyBadgeArea()
    property var rightBadgeChoices: emptyBadgeArea()
    readonly property bool customBadgeTextSupported: "customBadgeTextSupported" in runtime && runtime.customBadgeTextSupported
    property int brightnessDraft: runtime.brightness
    property bool mirrorDraft: runtime.mirrorMode
    property bool waterfallDraft: runtime.waterfallMode
    property string playMode: "Single"
    property string fullStylePosition: "Top"
    property string fullStyleColor: "#dcdcdc"
    property string fullStyleAlignment: "Left"
    property string leftStylePosition: "Top"
    property string leftStyleColor: "#dcdcdc"
    property string leftStyleAlignment: "Left"
    property string rightStylePosition: "Top"
    property string rightStyleColor: "#dcdcdc"
    property string rightStyleAlignment: "Right"
    property string pendingDeleteName: ""

    readonly property bool hasUnsavedChanges:
        confirmedSnapshotReady &&
        (deviceConflict ||
         isLayoutDraftDirty() ||
         isBrightnessDraftDirty() ||
         isOrientationDraftDirty())
    readonly property bool hasExternalConflict:
        layoutConflict || brightnessConflict ||
        orientationConflict || deviceConflict
    readonly property bool canApplyChanges:
        displayDraftCanApply()
    readonly property string applyBlockReason:
        displayDraftBlockReason()
    property bool applyPending: false
    property bool applyUnresolved: false
    property string loadedSavedLayoutId: ""
    property string loadedSavedLayoutRevision: ""
    property string loadedSavedLayoutDeviceIdentity: ""
    readonly property bool hasSavedLayoutBinding:
        loadedSavedLayoutId.length > 0

    property bool confirmedSnapshotReady: false
    property var confirmedLayout: ({})
    property var confirmedLayoutBuffers: ({})
    property int confirmedBrightness: 0
    property var confirmedOrientation: ({})
    property string confirmedDeviceIdentity: ""
    property bool layoutConflict: false
    property bool brightnessConflict: false
    property bool orientationConflict: false
    property bool deviceConflict: false
    property string activeDisplaySubmissionId: ""
    property var submittedDisplayDraft: null

    signal applyFinished(string outcome, string message)

    function copyList(value) {
        return value ? value.slice(0) : []
    }

    function copyBadgeChoice(value, trimText) {
        const mode = value ? String(value.mode || "Auto") : "Auto"
        const text = value ? String(value.text || "") : ""
        return {"mode": mode, "text": trimText && mode === "Custom" ? text.trim() : text}
    }

    function emptyBadgeArea() {
        return {"cpu": {"mode": "Auto", "text": ""}, "gpu": {"mode": "Auto", "text": ""}}
    }

    function badgeAreaFromEnvelope(value, secondary) {
        return {"cpu": copyBadgeChoice(value ? value[secondary ? "secondaryCpu" : "primaryCpu"] : null, false),
            "gpu": copyBadgeChoice(value ? value[secondary ? "secondaryGpu" : "primaryGpu"] : null, false)}
    }

    function copyBadgeArea(value) {
        return {"cpu": copyBadgeChoice(value ? value.cpu : null, false),
            "gpu": copyBadgeChoice(value ? value.gpu : null, false)}
    }

    function badgeEnvelope(primary, secondary, primaryIds, secondaryIds, split) {
        return {"schemaVersion": 1,
            "primaryCpu": copyBadgeChoice(primaryIds.indexOf("CPU Badge") >= 0 ? primary.cpu : null, true),
            "primaryGpu": copyBadgeChoice(primaryIds.indexOf("GPU Badge") >= 0 ? primary.gpu : null, true),
            "secondaryCpu": copyBadgeChoice(split && secondaryIds.indexOf("CPU Badge") >= 0 ? secondary.cpu : null, true),
            "secondaryGpu": copyBadgeChoice(split && secondaryIds.indexOf("GPU Badge") >= 0 ? secondary.gpu : null, true)}
    }

    function runtimeBadgeChoices() {
        const value = "displayBadgeChoices" in runtime ? runtime.displayBadgeChoices : null
        return badgeEnvelope(badgeAreaFromEnvelope(value, false), badgeAreaFromEnvelope(value, true),
            copyList(runtime.displayLeftBadges), copyList(runtime.displayRightBadges), runtime.currentScreenMode === "Screen Splitting")
    }

    function editBadgeChoice(area, slot, mode, text) {
        if (!confirmedSnapshotReady || applyPending || !customBadgeTextSupported)
            return
        const value = copyBadgeArea(area === "full" ? fullBadgeChoices : area === "left" ? leftBadgeChoices : rightBadgeChoices)
        value[slot] = {"mode": mode, "text": mode === "Auto" ? "" : text}
        if (area === "full") fullBadgeChoices = value
        else if (area === "left") leftBadgeChoices = value
        else rightBadgeChoices = value
    }

    function badgeAreaIsValid(area, ids) {
        for (const slot of ["cpu", "gpu"]) {
            if (ids.indexOf(slot === "cpu" ? "CPU Badge" : "GPU Badge") < 0)
                continue
            const choice = area[slot]
            if (choice.mode === "Custom" && (!customBadgeTextSupported || !("badgeTextError" in runtime)
                || runtime.badgeTextError(choice.mode, choice.text).length > 0))
                return false
        }
        return true
    }

    function activeBadgeChoicesAreValid() {
        return splitMode ? badgeAreaIsValid(leftBadgeChoices, leftBadges) && badgeAreaIsValid(rightBadgeChoices, rightBadges)
                         : badgeAreaIsValid(fullBadgeChoices, fullBadges)
    }

    function normalizedColor(value) {
        return String(value || "").toLowerCase()
    }

    function formatMediaSize(bytes) {
        let value = Number(bytes)
        if (!isFinite(value) || value <= 0)
            return qsTr("Unknown size")
        const units = [qsTr("B"), qsTr("KiB"),
                       qsTr("MiB"), qsTr("GiB")]
        let unit = 0
        while (value >= 1024 && unit < units.length - 1) {
            value /= 1024
            ++unit
        }
        return "%1 %2".arg(value.toFixed(unit === 0 ? 0 : 1))
                         .arg(units[unit])
    }

    function mediaOriginKind(source) {
        if (runtime.legacyConnected)
            return 0
        const normalized = Number(source)
        if (normalized === 1)
            return 1
        if (normalized === 2)
            return 2
        return 0
    }

    function mediaOriginLabel(source) {
        switch (mediaOriginKind(source)) {
        case 1:
            return qsTr("User media")
        case 2:
            return qsTr("Device preset")
        default:
            return qsTr("Unknown origin")
        }
    }

    function snapshotsEqual(left, right) {
        return JSON.stringify(left) === JSON.stringify(right)
    }

    function runtimeDeviceIdentity() {
        if ("displayDeviceIdentity" in runtime)
            return String(runtime.displayDeviceIdentity || "")
        return ""
    }

    function displayBaselineAvailable() {
        return runtime.displayStateValid ||
               runtime.legacyConnected
    }

    function displaySessionUsable() {
        return runtime.displaySessionActive ||
               runtime.legacyConnected
    }

    function displayIdentityAvailable() {
        return runtimeDeviceIdentity().length > 0
    }

    function legacyLayoutStateConfirmed() {
        if (!runtime.legacyConnected)
            return true
        if ("legacyDisplayLayoutConfirmed" in runtime)
            return runtime.legacyDisplayLayoutConfirmed
        return runtime.displayStateValid
    }

    function legacyBrightnessStateConfirmed() {
        if (!runtime.legacyConnected)
            return true
        if ("legacyDisplayBrightnessConfirmed" in runtime)
            return runtime.legacyDisplayBrightnessConfirmed
        return runtime.displayStateValid
    }

    function confirmedLayoutSnapshot() {
        const split = runtime.currentScreenMode ===
                      "Screen Splitting"
        const media = copyList(runtime.displayedMedia)
        const leftPosition = runtime.displayLeftPosition || "Top"
        const leftColor = normalizedColor(
            runtime.displayLeftColor || "#dcdcdc")
        const leftAlignment = runtime.displayLeftAlignment || "Left"
        const rightPosition = runtime.legacyConnected
                              ? leftPosition
                              : runtime.displayRightPosition || "Top"
        const rightColor = runtime.legacyConnected
                           ? leftColor
                           : normalizedColor(
                                 runtime.displayRightColor || "#dcdcdc")
        const rightAlignment = runtime.legacyConnected
                               ? leftAlignment
                               : runtime.displayRightAlignment || "Right"
        if (split) {
            return {
                "split": true,
                "media": media.slice(0, 2),
                "playMode": "Single",
                "leftMetrics": copyList(runtime.displayLeftMetrics),
                "rightMetrics": copyList(runtime.displayRightMetrics),
                "leftBadges": copyList(runtime.displayLeftBadges),
                "rightBadges": copyList(runtime.displayRightBadges),
                "badgeChoices": runtimeBadgeChoices(),
                "leftPosition": leftPosition,
                "leftColor": leftColor,
                "leftAlignment": leftAlignment,
                "rightPosition": rightPosition,
                "rightColor": rightColor,
                "rightAlignment": rightAlignment
            }
        }
        return {
            "split": false,
            "media": media.slice(0, 1),
            "playMode": runtime.currentPlayMode || "Single",
            "metrics": copyList(runtime.displayLeftMetrics),
            "badges": copyList(runtime.displayLeftBadges),
            "badgeChoices": runtimeBadgeChoices(),
            "position": leftPosition,
            "color": leftColor,
            "alignment": leftAlignment
        }
    }

    function confirmedLayoutBuffersSnapshot() {
        const leftPosition = runtime.displayLeftPosition || "Top"
        const leftColor = normalizedColor(
            runtime.displayLeftColor || "#dcdcdc")
        const leftAlignment = runtime.displayLeftAlignment || "Left"
        return {
            "fullMetrics": copyList(runtime.displayLeftMetrics),
            "leftMetrics": copyList(runtime.displayLeftMetrics),
            "rightMetrics": copyList(runtime.displayRightMetrics),
            "fullBadges": copyList(runtime.displayLeftBadges),
            "leftBadges": copyList(runtime.displayLeftBadges),
            "rightBadges": copyList(runtime.displayRightBadges),
            "fullBadgeChoices": badgeAreaFromEnvelope(runtimeBadgeChoices(), false),
            "leftBadgeChoices": badgeAreaFromEnvelope(runtimeBadgeChoices(), false),
            "rightBadgeChoices": badgeAreaFromEnvelope(runtimeBadgeChoices(), true),
            "fullPosition": leftPosition,
            "fullColor": leftColor,
            "fullAlignment": leftAlignment,
            "leftPosition": leftPosition,
            "leftColor": leftColor,
            "leftAlignment": leftAlignment,
            "rightPosition": runtime.legacyConnected
                             ? leftPosition
                             : runtime.displayRightPosition || "Top",
            "rightColor": runtime.legacyConnected
                          ? leftColor
                          : normalizedColor(
                                runtime.displayRightColor || "#dcdcdc"),
            "rightAlignment": runtime.legacyConnected
                              ? leftAlignment
                              : runtime.displayRightAlignment || "Right"
        }
    }

    function applyLayoutBuffers(snapshot) {
        fullMetrics = copyList(snapshot.fullMetrics)
        leftMetrics = copyList(snapshot.leftMetrics)
        rightMetrics = copyList(snapshot.rightMetrics)
        fullBadges = copyList(snapshot.fullBadges)
        leftBadges = copyList(snapshot.leftBadges)
        rightBadges = copyList(snapshot.rightBadges)
        fullBadgeChoices = copyBadgeArea(snapshot.fullBadgeChoices)
        leftBadgeChoices = copyBadgeArea(snapshot.leftBadgeChoices)
        rightBadgeChoices = copyBadgeArea(snapshot.rightBadgeChoices)
        fullStylePosition = snapshot.fullPosition
        fullStyleColor = snapshot.fullColor
        fullStyleAlignment = snapshot.fullAlignment
        leftStylePosition = snapshot.leftPosition
        leftStyleColor = snapshot.leftColor
        leftStyleAlignment = snapshot.leftAlignment
        rightStylePosition = snapshot.rightPosition
        rightStyleColor = snapshot.rightColor
        rightStyleAlignment = snapshot.rightAlignment
    }

    function activeDraftLayoutSnapshot() {
        if (splitMode) {
            const rightPosition = runtime.legacyConnected
                                  ? leftStylePosition
                                  : rightStylePosition
            const rightColor = runtime.legacyConnected
                               ? normalizedColor(leftStyleColor)
                               : normalizedColor(rightStyleColor)
            const rightAlignment = runtime.legacyConnected
                                   ? leftStyleAlignment
                                   : rightStyleAlignment
            return {
                "split": true,
                "media": copyList(selectedMedia).slice(0, 2),
                "playMode": "Single",
                "leftMetrics": copyList(leftMetrics),
                "rightMetrics": copyList(rightMetrics),
                "leftBadges": copyList(leftBadges),
                "rightBadges": copyList(rightBadges),
                "badgeChoices": badgeEnvelope(leftBadgeChoices, rightBadgeChoices, leftBadges, rightBadges, true),
                "leftPosition": leftStylePosition,
                "leftColor": normalizedColor(leftStyleColor),
                "leftAlignment": leftStyleAlignment,
                "rightPosition": rightPosition,
                "rightColor": rightColor,
                "rightAlignment": rightAlignment
            }
        }
        return {
            "split": false,
            "media": copyList(selectedMedia).slice(0, 1),
            "playMode": playMode || "Single",
            "metrics": copyList(fullMetrics),
            "badges": copyList(fullBadges),
            "badgeChoices": badgeEnvelope(fullBadgeChoices, emptyBadgeArea(), fullBadges, [], false),
            "position": fullStylePosition,
            "color": normalizedColor(fullStyleColor),
            "alignment": fullStyleAlignment
        }
    }

    function currentOrientationSnapshot() {
        return {
            "mirror": mirrorDraft,
            "waterfall": waterfallDraft
        }
    }

    function captureSavedLayoutFullState() {
        return {
            "layout": activeDraftLayoutSnapshot(),
            "brightness": brightnessDraft,
            "orientation": currentOrientationSnapshot()
        }
    }

    function clearSavedLayoutBinding() {
        loadedSavedLayoutId = ""
        loadedSavedLayoutRevision = ""
        loadedSavedLayoutDeviceIdentity = ""
    }

    function savedLayoutActionsAllowed() {
        return runtime.savedLayoutsReady &&
               confirmedSnapshotReady &&
               displayBaselineAvailable() &&
               displaySessionUsable() &&
               displayIdentityAvailable() &&
               !runtime.savedLayoutsBusy &&
               !runtime.operationBusy &&
               !applyPending && !applyUnresolved &&
               !hasExternalConflict
    }

    function savedLayoutActionsBlockReason() {
        if (!runtime.capabilitiesReady)
            return qsTr("Wait for the active runtime handshake.")
        if (!runtime.savedLayoutsSupported)
            return qsTr("Saved layouts are not supported by the active runtime.")
        if (!runtime.savedLayoutsReady)
            return qsTr("Wait for saved layouts from the current display.")
        if (!confirmedSnapshotReady ||
            !displayBaselineAvailable() ||
            !displaySessionUsable() ||
            !displayIdentityAvailable()) {
            return qsTr("Wait for a confirmed display state before using saved layouts.")
        }
        if (hasExternalConflict) {
            return qsTr("Discard the conflicted draft before using saved layouts.")
        }
        if (applyPending || applyUnresolved) {
            return qsTr("Resolve the current display Apply before using saved layouts.")
        }
        if (runtime.savedLayoutsBusy || runtime.operationBusy)
            return qsTr("Wait for the current request to finish.")
        return ""
    }

    function loadSavedLayoutIntoDraft(layoutId) {
        if (!savedLayoutActionsAllowed())
            return
        const draft = runtime.savedLayoutDraft(layoutId)
        if (!draft || String(draft.layoutId || "").length === 0)
            return
        applyLayoutSnapshot(draft.layout)
        brightnessDraft = Number(draft.brightness)
        mirrorDraft = Boolean(draft.orientation.mirror)
        waterfallDraft = Boolean(draft.orientation.waterfall)
        loadedSavedLayoutId = String(draft.layoutId)
        loadedSavedLayoutRevision = String(draft.revision)
        loadedSavedLayoutDeviceIdentity =
            String(draft.deviceIdentity)
    }

    function saveCurrentLayout(name, overwriteLayoutId) {
        if (!savedLayoutActionsAllowed() ||
            !activeLayoutIsValid(true))
            return
        runtime.putSavedLayout(
            name, overwriteLayoutId,
            captureSavedLayoutFullState())
    }

    function deleteSavedLayout(layoutId) {
        if (!savedLayoutActionsAllowed())
            return
        runtime.deleteSavedLayout(layoutId)
    }

    function confirmedOrientationSnapshot() {
        return {
            "mirror": runtime.mirrorMode,
            "waterfall": runtime.waterfallMode
        }
    }

    function isLayoutDraftDirty() {
        return confirmedSnapshotReady &&
               !snapshotsEqual(activeDraftLayoutSnapshot(),
                               confirmedLayout)
    }

    function isBrightnessDraftDirty() {
        return confirmedSnapshotReady &&
               brightnessDraft !== confirmedBrightness
    }

    function isOrientationDraftDirty() {
        return confirmedSnapshotReady &&
               !snapshotsEqual(currentOrientationSnapshot(),
                               confirmedOrientation)
    }

    function applyLayoutSnapshot(snapshot) {
        splitMode = snapshot.split
        selectedMedia = copyList(snapshot.media)
        if (snapshot.split) {
            playMode = "Single"
            leftMetrics = copyList(snapshot.leftMetrics)
            rightMetrics = copyList(snapshot.rightMetrics)
            leftBadges = copyList(snapshot.leftBadges)
            rightBadges = copyList(snapshot.rightBadges)
            leftBadgeChoices = badgeAreaFromEnvelope(snapshot.badgeChoices, false)
            rightBadgeChoices = badgeAreaFromEnvelope(snapshot.badgeChoices, true)
            leftStylePosition = snapshot.leftPosition
            leftStyleColor = snapshot.leftColor
            leftStyleAlignment = snapshot.leftAlignment
            rightStylePosition = snapshot.rightPosition
            rightStyleColor = snapshot.rightColor
            rightStyleAlignment = snapshot.rightAlignment
        } else {
            playMode = snapshot.playMode
            fullMetrics = copyList(snapshot.metrics)
            fullBadges = copyList(snapshot.badges)
            fullBadgeChoices = badgeAreaFromEnvelope(snapshot.badgeChoices, false)
            fullStylePosition = snapshot.position
            fullStyleColor = snapshot.color
            fullStyleAlignment = snapshot.alignment
        }
    }

    function initializeDisplayDraft() {
        if (!displayBaselineAvailable() ||
            !displaySessionUsable() ||
            !displayIdentityAvailable())
            return
        confirmedLayoutBuffers =
            confirmedLayoutBuffersSnapshot()
        applyLayoutBuffers(confirmedLayoutBuffers)
        confirmedLayout = confirmedLayoutSnapshot()
        confirmedBrightness = runtime.brightness
        confirmedOrientation = confirmedOrientationSnapshot()
        confirmedDeviceIdentity = runtimeDeviceIdentity()
        applyLayoutSnapshot(confirmedLayout)
        brightnessDraft = confirmedBrightness
        mirrorDraft = confirmedOrientation.mirror
        waterfallDraft = confirmedOrientation.waterfall
        confirmedSnapshotReady = true
    }

    function reconcileDisplayDraft() {
        if (!displayBaselineAvailable() ||
            !displaySessionUsable() ||
            !displayIdentityAvailable())
            return

        if (!confirmedSnapshotReady) {
            initializeDisplayDraft()
            return
        }

        const nextIdentity = runtimeDeviceIdentity()
        const identityChanged =
            nextIdentity !== confirmedDeviceIdentity
        if (runtime.legacyConnected &&
            !runtime.displayStateValid &&
            !legacyLayoutStateConfirmed() &&
            !identityChanged) {
            if (legacyBrightnessStateConfirmed()) {
                const nextBrightness = runtime.brightness
                const brightnessDirty =
                    brightnessDraft !== confirmedBrightness
                const brightnessAdvanced =
                    nextBrightness !== confirmedBrightness
                if (!brightnessDirty ||
                    brightnessDraft === nextBrightness) {
                    brightnessDraft = nextBrightness
                    brightnessConflict = false
                } else if (brightnessAdvanced) {
                    brightnessConflict = true
                }
                confirmedBrightness = nextBrightness
            }
            return
        }

        const nextLayout = confirmedLayoutSnapshot()
        const nextLayoutBuffers =
            confirmedLayoutBuffersSnapshot()
        const nextBrightness = runtime.legacyConnected &&
                               !legacyBrightnessStateConfirmed()
                               ? confirmedBrightness
                               : runtime.brightness
        const nextOrientation = confirmedOrientationSnapshot()

        if (identityChanged &&
            (hasUnsavedChanges || applyPending || applyUnresolved)) {
            confirmedLayout = nextLayout
            confirmedLayoutBuffers = nextLayoutBuffers
            confirmedBrightness = nextBrightness
            confirmedOrientation = nextOrientation
            confirmedDeviceIdentity = nextIdentity
            deviceConflict = true
            return
        }

        // A draft captured for another physical display cannot be rebased by
        // later state notifications. Only explicit Discard may adopt the
        // confirmed snapshot and clear this identity boundary.
        if (deviceConflict) {
            confirmedLayout = nextLayout
            confirmedLayoutBuffers = nextLayoutBuffers
            confirmedBrightness = nextBrightness
            confirmedOrientation = nextOrientation
            confirmedDeviceIdentity = nextIdentity
            return
        }

        const draftLayout = activeDraftLayoutSnapshot()
        const layoutDirty = !snapshotsEqual(draftLayout,
                                            confirmedLayout)
        const layoutAdvanced = !snapshotsEqual(nextLayout,
                                               confirmedLayout)
        if (!layoutDirty || snapshotsEqual(draftLayout, nextLayout)) {
            applyLayoutBuffers(nextLayoutBuffers)
            applyLayoutSnapshot(nextLayout)
            layoutConflict = false
        } else if (layoutAdvanced) {
            layoutConflict = true
        }

        const brightnessDirty = brightnessDraft !== confirmedBrightness
        const brightnessAdvanced = nextBrightness !== confirmedBrightness
        if (!brightnessDirty || brightnessDraft === nextBrightness) {
            brightnessDraft = nextBrightness
            brightnessConflict = false
        } else if (brightnessAdvanced) {
            brightnessConflict = true
        }

        const draftOrientation = currentOrientationSnapshot()
        const orientationDirty =
            !snapshotsEqual(draftOrientation, confirmedOrientation)
        const orientationAdvanced =
            !snapshotsEqual(nextOrientation, confirmedOrientation)
        if (!orientationDirty ||
            snapshotsEqual(draftOrientation, nextOrientation)) {
            mirrorDraft = nextOrientation.mirror
            waterfallDraft = nextOrientation.waterfall
            orientationConflict = false
        } else if (orientationAdvanced) {
            orientationConflict = true
        }

        confirmedLayout = nextLayout
        confirmedLayoutBuffers = nextLayoutBuffers
        confirmedBrightness = nextBrightness
        confirmedOrientation = nextOrientation
        confirmedDeviceIdentity = nextIdentity
    }

    function metricDraftIsValid(metrics) {
        if (!metrics || metrics.length > 3)
            return false
        const unique = []
        for (const metric of metrics) {
            if (unique.indexOf(metric) >= 0)
                return false
            unique.push(metric)
            if (!runtime.metricsCatalogReady ||
                runtime.metricsCatalog.indexOf(metric) < 0)
                return false
        }
        return true
    }

    function activeLayoutIsValid(layoutPresent) {
        if (!layoutPresent)
            return true
        if (!activeBadgeChoicesAreValid())
            return false
        if ((!splitMode && selectedMedia.length !== 1) ||
            (splitMode && selectedMedia.length !== 2))
            return false
        if (!splitMode)
            return metricDraftIsValid(fullMetrics) &&
                   fullStyleEditor.valid
        if (runtime.legacyConnected)
            return metricDraftIsValid(leftMetrics) &&
                   metricDraftIsValid(rightMetrics) &&
                   sharedStyleEditor.valid
        return metricDraftIsValid(leftMetrics) &&
               metricDraftIsValid(rightMetrics) &&
               leftStyleEditor.valid && rightStyleEditor.valid
    }

    function displayDraftCanApply() {
        if (!confirmedSnapshotReady || !hasUnsavedChanges ||
            hasExternalConflict || applyPending || applyUnresolved ||
            runtime.operationBusy || !displayBaselineAvailable() ||
            !displaySessionUsable() ||
            !displayIdentityAvailable())
            return false
        if (hasSavedLayoutBinding &&
            (!runtime.savedLayoutsReady ||
             runtime.savedLayoutsBusy ||
             runtime.savedLayoutsDeviceIdentity !==
             loadedSavedLayoutDeviceIdentity))
            return false

        const layoutPresent = isLayoutDraftDirty()
        const brightnessPresent = isBrightnessDraftDirty()
        const orientationPresent = isOrientationDraftDirty()
        if (runtime.legacyConnected) {
            if (mirrorDraft !== confirmedOrientation.mirror)
                return false
            const legacyLayoutPresent = layoutPresent ||
                                        orientationPresent
            if (brightnessPresent && legacyLayoutPresent)
                return false
            if (orientationPresent && !layoutPresent &&
                !legacyLayoutStateConfirmed())
                return false
            return activeLayoutIsValid(legacyLayoutPresent)
        }
        return activeLayoutIsValid(layoutPresent)
    }

    function displayDraftBlockReason() {
        if (!confirmedSnapshotReady ||
            !displayBaselineAvailable() ||
            !displaySessionUsable() ||
            !displayIdentityAvailable()) {
            return qsTr(
                "Reconnect the display and wait for a confirmed state before applying changes.")
        }
        if (!hasUnsavedChanges)
            return ""
        if (hasExternalConflict) {
            return qsTr(
                "The confirmed display state changed in the same area as this draft. Discard the draft to use the latest device state.")
        }
        if (applyPending)
            return qsTr("Display changes are being applied.")
        if (applyUnresolved) {
            return qsTr(
                "The display result is unknown. Review the device state or discard only the local draft.")
        }
        if (runtime.operationBusy) {
            return qsTr(
                "Wait for the current device operation to finish.")
        }
        if (hasSavedLayoutBinding &&
            (!runtime.savedLayoutsReady ||
             runtime.savedLayoutsBusy ||
             runtime.savedLayoutsDeviceIdentity !==
             loadedSavedLayoutDeviceIdentity)) {
            return qsTr(
                "Reload the saved layout from the current display before applying it.")
        }

        const layoutPresent = isLayoutDraftDirty()
        const brightnessPresent = isBrightnessDraftDirty()
        const orientationPresent = isOrientationDraftDirty()
        if (runtime.legacyConnected) {
            if (mirrorDraft !== confirmedOrientation.mirror) {
                return qsTr(
                    "Mirror changes are not supported by the legacy display connection.")
            }
            const legacyLayoutPresent = layoutPresent ||
                                        orientationPresent
            if (brightnessPresent && legacyLayoutPresent) {
                return qsTr(
                    "The legacy display cannot apply brightness together with layout or waterfall changes. Apply the sections separately or discard the draft.")
            }
            if (orientationPresent && !layoutPresent &&
                !legacyLayoutStateConfirmed()) {
                return qsTr(
                    "Apply a complete legacy layout before changing Waterfall by itself.")
            }
            if (!activeLayoutIsValid(legacyLayoutPresent)) {
                return qsTr(
                    "Select the required media and correct the invalid layout fields before applying changes.")
            }
            return ""
        }
        if (!activeLayoutIsValid(layoutPresent)) {
            return qsTr(
                "Select the required media and correct the invalid layout fields before applying changes.")
        }
        return ""
    }

    function captureDisplayDraft() {
        const layoutPresent = isLayoutDraftDirty()
        const brightnessPresent = isBrightnessDraftDirty()
        const orientationPresent = isOrientationDraftDirty()
        return {
            "layout": activeDraftLayoutSnapshot(),
            "brightness": brightnessDraft,
            "orientation": currentOrientationSnapshot(),
            "layoutPresent": runtime.legacyConnected
                             ? layoutPresent || orientationPresent
                             : layoutPresent,
            "brightnessPresent": brightnessPresent,
            "orientationPresent": orientationPresent
        }
    }

    function applyChanges() {
        if (!canApplyChanges)
            return

        const draft = hasSavedLayoutBinding
                      ? captureSavedLayoutFullState()
                      : captureDisplayDraft()
        if (hasSavedLayoutBinding) {
            draft.layoutPresent = true
            draft.brightnessPresent = true
            draft.orientationPresent = true
        }
        submittedDisplayDraft = draft
        applyPending = true
        applyUnresolved = false

        let submissionId = ""
        if (hasSavedLayoutBinding) {
            submissionId = runtime.submitSavedLayoutDraft(
                loadedSavedLayoutId,
                loadedSavedLayoutRevision,
                captureSavedLayoutFullState())
        } else if (draft.layout.split) {
            submissionId = runtime.submitSplitDisplayDraft(
                draft.layout.media[0], draft.layout.media[1],
                draft.layout.playMode,
                copyList(draft.layout.leftMetrics),
                copyList(draft.layout.rightMetrics),
                copyList(draft.layout.leftBadges),
                copyList(draft.layout.rightBadges),
                draft.layout.leftPosition,
                draft.layout.leftColor,
                draft.layout.leftAlignment,
                draft.layout.rightPosition,
                draft.layout.rightColor,
                draft.layout.rightAlignment,
                draft.layoutPresent,
                draft.brightnessPresent, draft.brightness,
                draft.orientationPresent,
                draft.orientation.mirror,
                draft.orientation.waterfall, draft.layout.badgeChoices)
        } else {
            submissionId = runtime.submitFullDisplayDraft(
                copyList(draft.layout.media),
                draft.layout.playMode,
                copyList(draft.layout.metrics),
                copyList(draft.layout.badges),
                draft.layout.position,
                draft.layout.color,
                draft.layout.alignment,
                draft.layoutPresent,
                draft.brightnessPresent, draft.brightness,
                draft.orientationPresent,
                draft.orientation.mirror,
                draft.orientation.waterfall, draft.layout.badgeChoices)
        }

        activeDisplaySubmissionId = String(submissionId || "")
        if (activeDisplaySubmissionId.length === 0) {
            applyPending = false
            submittedDisplayDraft = null
            applyFinished("Rejected",
                          qsTr("The display draft could not be submitted."))
        }
    }

    function discardChanges() {
        if (applyPending || !confirmedSnapshotReady)
            return
        if (activeDisplaySubmissionId.length > 0 &&
            typeof runtime.abandonDisplaySubmission === "function") {
            runtime.abandonDisplaySubmission(
                activeDisplaySubmissionId)
        }
        applyLayoutBuffers(confirmedLayoutBuffers)
        applyLayoutSnapshot(confirmedLayout)
        brightnessDraft = confirmedBrightness
        mirrorDraft = confirmedOrientation.mirror
        waterfallDraft = confirmedOrientation.waterfall
        layoutConflict = false
        brightnessConflict = false
        orientationConflict = false
        deviceConflict = false
        applyUnresolved = false
        activeDisplaySubmissionId = ""
        submittedDisplayDraft = null
        clearSavedLayoutBinding()
    }

    function submittedDraftIsStillCurrent() {
        if (!submittedDisplayDraft)
            return false
        const draft = submittedDisplayDraft
        if (draft.layoutPresent &&
            !snapshotsEqual(activeDraftLayoutSnapshot(), draft.layout))
            return false
        if (draft.brightnessPresent &&
            brightnessDraft !== draft.brightness)
            return false
        if (draft.orientationPresent &&
            !snapshotsEqual(currentOrientationSnapshot(),
                            draft.orientation))
            return false
        return true
    }

    function toggled(list, value, limit) {
        const next = list.slice(0)
        const existing = next.indexOf(value)
        if (existing >= 0) {
            next.splice(existing, 1)
            return next
        }
        if (next.length >= limit)
            next.shift()
        next.push(value)
        return next
    }

    function toggleMedia(value) {
        if (!confirmedSnapshotReady)
            return
        selectedMedia = toggled(selectedMedia, value,
                                splitMode ? 2 : 1)
    }

    function toggleBadge(group, value) {
        if (!confirmedSnapshotReady || applyPending)
            return
        if (group === "full")
            fullBadges = toggled(fullBadges, value, 2)
        else if (group === "left")
            leftBadges = toggled(leftBadges, value, 2)
        else
            rightBadges = toggled(rightBadges, value, 2)
        if (!badgeSelected(group, value)) {
            const choices = copyBadgeArea(group === "full" ? fullBadgeChoices : group === "left" ? leftBadgeChoices : rightBadgeChoices)
            choices[value === "CPU Badge" ? "cpu" : "gpu"] = {"mode": "Auto", "text": ""}
            if (group === "full") fullBadgeChoices = choices
            else if (group === "left") leftBadgeChoices = choices
            else rightBadgeChoices = choices
        }
    }

    function requestDelete(mediaName) {
        if (!runtime.mediaModel.canDelete(mediaName))
            return
        pendingDeleteName = mediaName
        deleteConfirmation.open()
    }

    function badgeSelected(group, value) {
        if (group === "full")
            return fullBadges.indexOf(value) >= 0
        if (group === "left")
            return leftBadges.indexOf(value) >= 0
        return rightBadges.indexOf(value) >= 0
    }

    function synchronizeDisplayDraft() {
        reconcileDisplayDraft()
    }

    Component.onCompleted: initializeDisplayDraft()

    Connections {
        target: root.runtime
        ignoreUnknownSignals: true
        function onDisplayChanged() {
            root.reconcileDisplayDraft()
        }
        function onDisplayDeviceIdentityChanged() {
            root.reconcileDisplayDraft()
        }
        function onConnectionChanged() {
            if (root.hasSavedLayoutBinding &&
                (!root.runtime.savedLayoutsReady ||
                 root.runtime.savedLayoutsDeviceIdentity !==
                 root.loadedSavedLayoutDeviceIdentity ||
                 root.runtimeDeviceIdentity() !==
                 root.loadedSavedLayoutDeviceIdentity)) {
                root.clearSavedLayoutBinding()
            }
            root.reconcileDisplayDraft()
        }
        function onRuntimeInvalidated() {
            root.clearSavedLayoutBinding()
        }
        function onSavedLayoutsChanged() {
            if (!root.hasSavedLayoutBinding ||
                !root.runtime.savedLayoutsReady)
                return
            if (root.runtime.savedLayoutsDeviceIdentity !==
                root.loadedSavedLayoutDeviceIdentity ||
                String(root.runtime.savedLayoutDraft(
                    root.loadedSavedLayoutId).layoutId || "")
                    .length === 0) {
                root.clearSavedLayoutBinding()
            }
        }
        function onSavedLayoutPutFinished(
            requestedLayoutId, savedLayoutId,
            revisionDecimal, success, message) {
            if (!success || requestedLayoutId.length === 0 ||
                requestedLayoutId !== root.loadedSavedLayoutId)
                return
            root.loadedSavedLayoutId = savedLayoutId
            root.loadedSavedLayoutRevision = revisionDecimal
            root.loadedSavedLayoutDeviceIdentity =
                root.runtime.savedLayoutsDeviceIdentity
        }
        function onSavedLayoutDeleteFinished(
            layoutId, success, message) {
            if (success &&
                layoutId === root.loadedSavedLayoutId)
                root.clearSavedLayoutBinding()
        }
        function onDisplayApplyFinished(submissionId,
                                        outcome,
                                        message) {
            if (submissionId !==
                root.activeDisplaySubmissionId)
                return

            const draftStillCurrent =
                root.submittedDraftIsStillCurrent()
            root.applyPending = false
            if (outcome === "PartialOrUnknown" ||
                outcome === "Unresolved") {
                root.applyUnresolved = true
            } else {
                root.applyUnresolved = false
                root.activeDisplaySubmissionId = ""
            }

            if (outcome === "Succeeded") {
                root.reconcileDisplayDraft()
                if (!draftStillCurrent ||
                    root.hasUnsavedChanges) {
                    root.applyFinished(
                        "Succeeded",
                        message)
                    root.submittedDisplayDraft = null
                    return
                }
                root.layoutConflict = false
                root.brightnessConflict = false
                root.orientationConflict = false
                root.deviceConflict = false
            }

            if (!root.applyUnresolved)
                root.submittedDisplayDraft = null
            root.applyFinished(outcome, message)
        }
    }

    ColumnLayout {
        objectName: "panoramaContent"
        x: 24
        y: 24
        width: Math.max(0, root.availableWidth - 48)
        spacing: 16

        OperationBanner {
            Layout.fillWidth: true
            runtime: root.runtime
        }

        GridLayout {
            id: displayWorkspace

            objectName: "displayWorkspace"
            Layout.fillWidth: true
            columns: root.availableWidth >= 1120 ? 2 : 1
            columnSpacing: 16
            rowSpacing: 16

            ColumnLayout {
                Layout.fillWidth: true
                Layout.alignment: Qt.AlignTop
                Layout.minimumWidth:
                    displayWorkspace.columns === 2 ? 600 : 0
                Layout.preferredWidth:
                    displayWorkspace.columns === 2 ? 700 : 0
                spacing: 16

                RowLayout {
                    Layout.fillWidth: true
                    spacing: 10
                    PrimaryButton {
                        objectName: "uploadMediaButton"
                        text: qsTr("Upload media…")
                        enabled: root.runtime.displaySessionActive &&
                                 !root.runtime.operationBusy
                        onClicked: mediaPicker.openPicker()
                    }
                    Button {
                        text: qsTr("Reload media library")
                        enabled: root.runtime.compatible &&
                                 !root.runtime.operationBusy
                        onClicked: root.runtime.refreshMedia()
                    }
                    Item { Layout.fillWidth: true }
                    Label {
                        text: qsTr("%1 selected")
                              .arg(root.selectedMedia.length)
                        color: "#9da1b3"
                    }
                }

                Frame {
                    id: dropFrame

                    Layout.fillWidth: true
                    Layout.preferredHeight: 72
                    background: Rectangle {
                        radius: 8
                        color: dropArea.containsDrag
                               ? "#30372d" : "#23282d"
                        border.width: 2
                        border.color: dropArea.containsDrag
                                      ? "#def750" : "#4a535a"
                    }
                    Label {
                        anchors.centerIn: parent
                        width: parent.width - 24
                        text: qsTr("Drop one MP4, WebM, MKV, AVI, MOV, GIF, JPG, PNG, BMP or WebP file here")
                        color: "#b8bbca"
                        horizontalAlignment: Text.AlignHCenter
                        elide: Text.ElideRight
                    }
                    DropArea {
                        id: dropArea
                        anchors.fill: parent
                        enabled: !root.runtime.operationBusy
                        onDropped: drop => {
                            root.editor.beginDropped(drop.urls)
                            drop.acceptProposedAction()
                        }
                    }
                }

                GroupBox {
                    objectName: "mediaLibraryGroup"
                    Layout.fillWidth: true
                    Layout.preferredHeight: Math.min(
                        470,
                        Math.max(
                            235,
                            Math.ceil(
                                mediaGrid.count /
                                Math.max(
                                    1,
                                    Math.floor(
                                        mediaGrid.width /
                                        mediaGrid.cellWidth))) *
                            mediaGrid.cellHeight + 45))
                    title: qsTr("Media Library")

                    GridView {
                        id: mediaGrid

                        objectName: "mediaGrid"
                        anchors.fill: parent
                        cellWidth: 220
                        cellHeight: 158
                        clip: true
                        model: root.runtime.mediaModel

                        delegate: ItemDelegate {
                            id: mediaDelegate

                            required property string mediaName
                            required property string mediaId
                            required property var thumbnailUrl
                            required property bool deleteAllowed
                            required property string deleteBlockReason
                            required property bool deviceCopyAllowed
                            required property string deviceCopyBlockReason
                            required property double mediaSize
                            required property int mediaSource

                            readonly property int originKind:
                                root.mediaOriginKind(mediaSource)
                            readonly property string originLabel:
                                root.mediaOriginLabel(mediaSource)
                            readonly property string formattedSize:
                                root.formatMediaSize(mediaSize)

                            objectName: "mediaCard-" + mediaId
                            width: mediaGrid.cellWidth - 10
                            height: mediaGrid.cellHeight - 10
                            activeFocusOnTab: true
                            highlighted:
                                root.selectedMedia.indexOf(mediaName) >= 0
                            Accessible.name: mediaName
                            Accessible.description:
                                originLabel + ", " + formattedSize
                            Accessible.selectable: true
                            Accessible.selected: highlighted
                            onClicked: root.toggleMedia(mediaName)
                            Keys.onReturnPressed: event => {
                                root.toggleMedia(mediaName)
                                event.accepted = true
                            }
                            Keys.onEnterPressed: event => {
                                root.toggleMedia(mediaName)
                                event.accepted = true
                            }
                            ToolTip.visible: hovered &&
                                             !deleteAllowed &&
                                             deleteBlockReason.length > 0
                            ToolTip.text: deleteBlockReason

                            contentItem: ColumnLayout {
                                spacing: 6
                                Rectangle {
                                    Layout.fillWidth: true
                                    Layout.preferredHeight: 92
                                    color: "#15181b"
                                    radius: 5
                                    Image {
                                        anchors.fill: parent
                                        anchors.margins: 3
                                        source: mediaDelegate.thumbnailUrl
                                        fillMode: Image.PreserveAspectFit
                                        asynchronous: true
                                    }
                                    Label {
                                        anchors.centerIn: parent
                                        visible:
                                            String(mediaDelegate.thumbnailUrl)
                                            .length === 0
                                        text: qsTr("No preview")
                                        color: "#6f7383"
                                    }
                                    Rectangle {
                                        id: mediaOriginBadge

                                        objectName:
                                            "mediaOriginBadge-" +
                                            mediaDelegate.mediaId
                                        anchors.left: parent.left
                                        anchors.top: parent.top
                                        anchors.margins: 6
                                        width: Math.max(
                                            0,
                                            Math.min(
                                                mediaOriginText.implicitWidth +
                                                16,
                                                mediaActionButton.x - x - 6))
                                        height: 22
                                        radius: 5
                                        color:
                                            mediaDelegate.originKind === 1
                                            ? "#def750"
                                            : mediaDelegate.originKind === 2
                                              ? "#343a3f"
                                              : "#352d20"
                                        border.width: 1
                                        border.color:
                                            mediaDelegate.originKind === 1
                                            ? "#a9bd42"
                                            : mediaDelegate.originKind === 2
                                              ? "#6b6f80"
                                              : "#8a6c3f"

                                        Label {
                                            id: mediaOriginText

                                            objectName:
                                                "mediaOriginLabel-" +
                                                mediaDelegate.mediaId
                                            anchors.fill: parent
                                            anchors.leftMargin: 8
                                            anchors.rightMargin: 8
                                            text: mediaDelegate.originLabel
                                            color:
                                                mediaDelegate.originKind === 1
                                                ? "#171a1e"
                                                : mediaDelegate.originKind === 2
                                                  ? "#f4f6f7"
                                                  : "#f0c27b"
                                            font.pixelSize: 9
                                            font.bold: true
                                            elide: Text.ElideRight
                                            horizontalAlignment:
                                                Text.AlignHCenter
                                            verticalAlignment:
                                                Text.AlignVCenter
                                            Accessible.ignored: true
                                        }
                                    }
                                    ToolButton {
                                        id: mediaActionButton

                                        objectName: "mediaActionButton"
                                        anchors.top: parent.top
                                        anchors.right: parent.right
                                        anchors.margins: 6
                                        width: 30
                                        height: 30
                                        padding: 0
                                        leftPadding: 0
                                        rightPadding: 0
                                        topPadding: 0
                                        bottomPadding: 0
                                        leftInset: 0
                                        rightInset: 0
                                        topInset: 0
                                        bottomInset: 0
                                        text: "⋯"
                                        Accessible.name:
                                            qsTr("Media actions")
                                        enabled:
                                            !root.runtime.operationBusy &&
                                            !root.deviceMedia.busy
                                        ToolTip.visible: hovered
                                        ToolTip.text:
                                            !mediaDelegate.deviceCopyAllowed &&
                                            mediaDelegate
                                                .deviceCopyBlockReason
                                                .length > 0
                                            ? mediaDelegate
                                                .deviceCopyBlockReason
                                            : qsTr("Media actions")
                                        background: Rectangle {
                                            anchors.fill: parent
                                            radius: width / 2
                                            color:
                                                mediaActionButton.hovered
                                                ? "#485057"
                                                : "#343a3f"
                                            border.width: 1
                                            border.color:
                                                mediaActionButton.hovered
                                                ? "#8d979f"
                                                : "#515960"
                                        }
                                        contentItem: Label {
                                            text: mediaActionButton.text
                                            color: "#f4f6f7"
                                            font.pixelSize: 20
                                            horizontalAlignment:
                                                Text.AlignHCenter
                                            verticalAlignment:
                                                Text.AlignVCenter
                                        }
                                        onClicked: mediaActions.open()

                                        Menu {
                                            id: mediaActions

                                            y: mediaActionButton.height + 4

                                            MenuItem {
                                                objectName:
                                                    "editDeviceMediaAction"
                                                text: qsTr("Edit")
                                                enabled:
                                                    mediaDelegate
                                                        .deviceCopyAllowed
                                                onTriggered:
                                                    root.deviceMedia
                                                        .beginEdit(
                                                            mediaDelegate
                                                                .mediaId,
                                                            mediaDelegate
                                                                .mediaName)
                                            }

                                            MenuItem {
                                                objectName:
                                                    "exportDeviceMediaAction"
                                                text: qsTr("Export copy…")
                                                enabled:
                                                    mediaDelegate
                                                        .deviceCopyAllowed
                                                onTriggered:
                                                    exportPicker.openFor(
                                                        mediaDelegate.mediaId,
                                                        mediaDelegate
                                                            .mediaName)
                                            }

                                            MenuSeparator {}

                                            MenuItem {
                                                objectName:
                                                    "deleteDeviceMediaAction"
                                                text: qsTr("Delete")
                                                enabled:
                                                    mediaDelegate
                                                        .deleteAllowed
                                                onTriggered:
                                                    root.requestDelete(
                                                        mediaDelegate
                                                            .mediaName)
                                            }
                                        }
                                    }
                                }
                                Label {
                                    Layout.fillWidth: true
                                    text: mediaDelegate.mediaName
                                    elide: Text.ElideMiddle
                                    font.pixelSize: 12
                                }
                                Label {
                                    objectName:
                                        "mediaSizeLabel-" +
                                        mediaDelegate.mediaId
                                    text: mediaDelegate.formattedSize
                                    color: "#85899a"
                                    font.pixelSize: 10
                                }
                            }
                        }

                        Label {
                            objectName: "mediaLibraryEmptyState"
                            anchors.centerIn: parent
                            visible: mediaGrid.count === 0
                            text: qsTr("No media available")
                            color: "#8d91a1"
                        }
                    }
                }
            }

            ColumnLayout {
                Layout.fillWidth: true
                Layout.alignment: Qt.AlignTop
                Layout.minimumWidth:
                    displayWorkspace.columns === 2 ? 400 : 0
                Layout.preferredWidth:
                    displayWorkspace.columns === 2 ? 430 : 0
                spacing: 16

            SavedLayoutsPanel {
                id: savedLayoutsPanel

                objectName: "savedLayoutsPanel"
                Layout.fillWidth: true
                runtime: root.runtime
                actionsAllowed: root.savedLayoutActionsAllowed()
                saveAllowed: actionsAllowed &&
                             root.activeLayoutIsValid(true)
                actionsBlockReason:
                    root.savedLayoutActionsBlockReason()
                draftDirty: root.hasUnsavedChanges
                onSaveRequested: (name, overwriteLayoutId) =>
                    root.saveCurrentLayout(name, overwriteLayoutId)
                onLoadRequested: layoutId =>
                    root.loadSavedLayoutIntoDraft(layoutId)
                onDeleteRequested: layoutId =>
                    root.deleteSavedLayout(layoutId)
            }

            GroupBox {
            objectName: "displayLayoutGroup"
            Layout.fillWidth: true
            enabled: root.confirmedSnapshotReady
            title: qsTr("Display layout")

            ColumnLayout {
                anchors.fill: parent
                spacing: 12

                GridLayout {
                    Layout.fillWidth: true
                    columns: 2
                    columnSpacing: 10
                    rowSpacing: 8

                    RadioButton {
                        Layout.fillWidth: true
                        text: qsTr("Full screen")
                        checked: !root.splitMode
                        onClicked: {
                            root.splitMode = false
                            if (root.selectedMedia.length > 1)
                                root.selectedMedia =
                                    [root.selectedMedia[0]]
                        }
                    }
                    RadioButton {
                        Layout.fillWidth: true
                        text: qsTr("Split screen")
                        checked: root.splitMode
                        onClicked: root.splitMode = true
                    }
                    Label { text: qsTr("Play mode") }
                    ComboBox {
                        id: playModeCombo
                        objectName: "playModeCombo"
                        Layout.fillWidth: true

                        readonly property var playModes:
                            root.splitMode
                            ? [
                                {"value": "Single",
                                 "label": qsTr("Single")}
                              ]
                            : [
                                {"value": "Single",
                                 "label": qsTr("Single")},
                                {"value": "Loop",
                                 "label": qsTr("Loop")},
                                {"value": "Shuffle",
                                 "label": qsTr("Shuffle")}
                              ]
                        model: playModes
                        textRole: "label"
                        currentIndex: Math.max(
                            0,
                            playModes.findIndex(
                                item => item.value ===
                                (root.splitMode
                                 ? "Single"
                                 : root.playMode)))
                        onActivated: index => {
                            root.playMode =
                                playModes[index].value
                        }
                    }
                }

                Label {
                    objectName: "mediaSelectionSummary"
                    Layout.fillWidth: true
                    Layout.minimumWidth: 0
                    text: root.splitMode
                          ? qsTr("Left: %1    Right: %2")
                            .arg(root.selectedMedia[0] || qsTr("not selected"))
                            .arg(root.selectedMedia[1] || qsTr("not selected"))
                          : qsTr("Media: %1")
                            .arg(root.selectedMedia[0] ||
                                 qsTr("not selected"))
                    color: "#b8bbca"
                    elide: Text.ElideMiddle
                }

                Label {
                    Layout.fillWidth: true
                    Layout.minimumWidth: 0
                    text: root.splitMode
                          ? qsTr("Select up to three metrics per side")
                          : qsTr("Select up to three overlay metrics")
                    font.bold: true
                    wrapMode: Text.WordWrap
                }

                MetricSelector {
                    id: fullMetricSelector

                    objectName: "fullMetricSelector"
                    Layout.fillWidth: true
                    visible: !root.splitMode
                    areaTitle: qsTr("Full")
                    fieldPrefix: "full"
                    catalog: root.runtime.metricsCatalog
                    catalogReady: root.runtime.metricsCatalogReady
                    availableMetrics: root.runtime.availableMetrics
                    selectedMetrics: root.fullMetrics
                    editable: root.confirmedSnapshotReady &&
                              !root.applyPending
                    onSelectionEdited: metrics =>
                        root.fullMetrics = metrics
                }

                Label {
                    visible: !root.splitMode
                    text: qsTr("Hardware badges")
                    font.bold: true
                }

                Flow {
                    Layout.fillWidth: true
                    spacing: 8
                    visible: !root.splitMode

                    CheckBox {
                        objectName: "fullCpuBadge"
                        text: qsTr("CPU Badge")
                        checked:
                            root.badgeSelected(
                                "full", "CPU Badge")
                        onClicked:
                            root.toggleBadge(
                                "full", "CPU Badge")
                    }
                    CheckBox {
                        objectName: "fullGpuBadge"
                        text: qsTr("GPU Badge")
                        checked:
                            root.badgeSelected(
                                "full", "GPU Badge")
                        onClicked:
                            root.toggleBadge(
                                "full", "GPU Badge")
                    }
                }

                Repeater {
                    model: ["cpu", "gpu"]
                    delegate: BadgeTextEditor {
                        required property string modelData
                        runtime: root.runtime
                        badgeTitle: modelData === "cpu" ? qsTr("CPU Badge") : qsTr("GPU Badge")
                        fieldPrefix: "full" + (modelData === "cpu" ? "Cpu" : "Gpu")
                        choice: root.fullBadgeChoices[modelData]
                        visible: !root.splitMode && (root.customBadgeTextSupported || choice.mode === "Custom")
                            && root.badgeSelected("full", modelData === "cpu" ? "CPU Badge" : "GPU Badge")
                        editable: root.customBadgeTextSupported && root.confirmedSnapshotReady && !root.applyPending
                        onChoiceEdited: (mode, text) => root.editBadgeChoice("full", modelData, mode, text)
                    }
                }

                GridLayout {
                    id: splitMetricGrid

                    objectName: "splitMetricGrid"
                    Layout.fillWidth: true
                    Layout.preferredWidth: parent
                                           ? parent.width
                                           : implicitWidth
                    visible: root.splitMode
                    columns: parent && parent.width >= 620 ? 2 : 1
                    columnSpacing: 10
                    rowSpacing: 10
                    ColumnLayout {
                        Layout.fillWidth: true
                        Layout.minimumWidth: 0
                        MetricSelector {
                            id: leftMetricSelector

                            objectName: "leftMetricSelector"
                            Layout.fillWidth: true
                            areaTitle: qsTr("Left")
                            fieldPrefix: "left"
                            catalog: root.runtime.metricsCatalog
                            catalogReady:
                                root.runtime.metricsCatalogReady
                            availableMetrics:
                                root.runtime.availableMetrics
                            selectedMetrics: root.leftMetrics
                            editable: root.confirmedSnapshotReady &&
                                      !root.applyPending
                            onSelectionEdited: metrics =>
                                root.leftMetrics = metrics
                        }
                        Label {
                            text: qsTr("Left badges")
                            font.bold: true
                        }
                        Flow {
                            Layout.fillWidth: true
                            spacing: 6

                            CheckBox {
                                objectName: "leftCpuBadge"
                                text: qsTr("CPU Badge")
                                checked:
                                    root.badgeSelected(
                                        "left", "CPU Badge")
                                onClicked:
                                    root.toggleBadge(
                                        "left", "CPU Badge")
                            }
                            CheckBox {
                                objectName: "leftGpuBadge"
                                text: qsTr("GPU Badge")
                                checked:
                                    root.badgeSelected(
                                        "left", "GPU Badge")
                                onClicked:
                                root.toggleBadge(
                                        "left", "GPU Badge")
                            }
                        }
                        Repeater {
                            model: ["cpu", "gpu"]
                            delegate: BadgeTextEditor {
                                required property string modelData
                                runtime: root.runtime
                                badgeTitle: modelData === "cpu" ? qsTr("Left CPU badge") : qsTr("Left GPU badge")
                                fieldPrefix: "left" + (modelData === "cpu" ? "Cpu" : "Gpu")
                                choice: root.leftBadgeChoices[modelData]
                                visible: (root.customBadgeTextSupported || choice.mode === "Custom")
                                    && root.badgeSelected("left", modelData === "cpu" ? "CPU Badge" : "GPU Badge")
                                editable: root.customBadgeTextSupported && root.confirmedSnapshotReady && !root.applyPending
                                onChoiceEdited: (mode, text) => root.editBadgeChoice("left", modelData, mode, text)
                            }
                        }
                    }
                    ColumnLayout {
                        Layout.fillWidth: true
                        Layout.minimumWidth: 0
                        MetricSelector {
                            id: rightMetricSelector

                            objectName: "rightMetricSelector"
                            Layout.fillWidth: true
                            areaTitle: qsTr("Right")
                            fieldPrefix: "right"
                            catalog: root.runtime.metricsCatalog
                            catalogReady:
                                root.runtime.metricsCatalogReady
                            availableMetrics:
                                root.runtime.availableMetrics
                            selectedMetrics: root.rightMetrics
                            editable: root.confirmedSnapshotReady &&
                                      !root.applyPending
                            onSelectionEdited: metrics =>
                                root.rightMetrics = metrics
                        }
                        Label {
                            text: qsTr("Right badges")
                            font.bold: true
                        }
                        Flow {
                            Layout.fillWidth: true
                            spacing: 6

                            CheckBox {
                                objectName: "rightCpuBadge"
                                text: qsTr("CPU Badge")
                                checked:
                                    root.badgeSelected(
                                        "right", "CPU Badge")
                                onClicked:
                                    root.toggleBadge(
                                        "right", "CPU Badge")
                            }
                            CheckBox {
                                objectName: "rightGpuBadge"
                                text: qsTr("GPU Badge")
                                checked:
                                    root.badgeSelected(
                                        "right", "GPU Badge")
                                onClicked:
                                root.toggleBadge(
                                        "right", "GPU Badge")
                            }
                        }
                        Repeater {
                            model: ["cpu", "gpu"]
                            delegate: BadgeTextEditor {
                                required property string modelData
                                runtime: root.runtime
                                badgeTitle: modelData === "cpu" ? qsTr("Right CPU badge") : qsTr("Right GPU badge")
                                fieldPrefix: "right" + (modelData === "cpu" ? "Cpu" : "Gpu")
                                choice: root.rightBadgeChoices[modelData]
                                visible: (root.customBadgeTextSupported || choice.mode === "Custom")
                                    && root.badgeSelected("right", modelData === "cpu" ? "CPU Badge" : "GPU Badge")
                                editable: root.customBadgeTextSupported && root.confirmedSnapshotReady && !root.applyPending
                                onChoiceEdited: (mode, text) => root.editBadgeChoice("right", modelData, mode, text)
                            }
                        }
                    }
                }

                Rectangle {
                    Layout.fillWidth: true
                    Layout.preferredHeight: 1
                    color: "#343a40"
                }

                Label {
                    Layout.fillWidth: true
                    visible: root.customBadgeTextSupported
                    text: qsTr("Custom text uses a neutral background. Cyrillic and other glyphs have not been verified on the device.")
                    textFormat: Text.PlainText
                    wrapMode: Text.WordWrap
                    color: "#b8bbca"
                }

                OverlayStyleEditor {
                    id: fullStyleEditor

                    objectName: "fullStyleEditor"
                    Layout.fillWidth: true
                    activeForMode: !root.splitMode
                    sectionTitle: qsTr("Full-screen overlay style")
                    fieldPrefix: "full"
                    placement: root.fullStylePosition
                    textColor: root.fullStyleColor
                    alignment: root.fullStyleAlignment
                    showPlacement: root.runtime.waterfallMode
                    onPlacementEdited: value =>
                        root.fullStylePosition = value
                    onTextColorEdited: value =>
                        root.fullStyleColor = value
                    onAlignmentEdited: value =>
                        root.fullStyleAlignment = value
                }

                Label {
                    Layout.fillWidth: true
                    Layout.minimumWidth: 0
                    visible: !root.splitMode &&
                             !root.runtime.waterfallMode
                    text: qsTr("Top/Bottom position becomes available after Waterfall orientation is confirmed.")
                    color: "#8d91a1"
                    wrapMode: Text.WordWrap
                }

                GridLayout {
                    id: splitStyleGrid

                    objectName: "splitStyleGrid"
                    Layout.fillWidth: true
                    Layout.preferredWidth: parent
                                           ? parent.width
                                           : implicitWidth
                    visible: root.splitMode &&
                             !root.runtime.legacyConnected
                    columns: parent && parent.width >= 620 ? 2 : 1
                    columnSpacing: 10
                    rowSpacing: 10

                    OverlayStyleEditor {
                        id: leftStyleEditor

                        objectName: "leftStyleEditor"
                        Layout.fillWidth: true
                        activeForMode: root.splitMode &&
                                       !root.runtime.legacyConnected
                        sectionTitle: qsTr("Left-side overlay style")
                        fieldPrefix: "left"
                        placement: root.leftStylePosition
                        textColor: root.leftStyleColor
                        alignment: root.leftStyleAlignment
                        showPlacement: false
                        onPlacementEdited: value =>
                            root.leftStylePosition = value
                        onTextColorEdited: value =>
                            root.leftStyleColor = value
                        onAlignmentEdited: value =>
                            root.leftStyleAlignment = value
                    }

                    OverlayStyleEditor {
                        id: rightStyleEditor

                        objectName: "rightStyleEditor"
                        Layout.fillWidth: true
                        activeForMode: root.splitMode &&
                                       !root.runtime.legacyConnected
                        sectionTitle: qsTr("Right-side overlay style")
                        fieldPrefix: "right"
                        placement: root.rightStylePosition
                        textColor: root.rightStyleColor
                        alignment: root.rightStyleAlignment
                        showPlacement: false
                        onPlacementEdited: value =>
                            root.rightStylePosition = value
                        onTextColorEdited: value =>
                            root.rightStyleColor = value
                        onAlignmentEdited: value =>
                            root.rightStyleAlignment = value
                    }
                }

                OverlayStyleEditor {
                    id: sharedStyleEditor

                    objectName: "sharedStyleEditor"
                    Layout.fillWidth: true
                    activeForMode: root.splitMode &&
                                   root.runtime.legacyConnected
                    sectionTitle: qsTr("Shared split overlay style")
                    fieldPrefix: "shared"
                    placement: root.leftStylePosition
                    textColor: root.leftStyleColor
                    alignment: root.leftStyleAlignment
                    showPlacement: false
                    onPlacementEdited: value =>
                        root.leftStylePosition = value
                    onTextColorEdited: value =>
                        root.leftStyleColor = value
                    onAlignmentEdited: value =>
                        root.leftStyleAlignment = value
                }

                Label {
                    Layout.fillWidth: true
                    Layout.minimumWidth: 0
                    visible: root.splitMode
                    text: qsTr("Split sides use fixed device placement; only color and alignment are editable.")
                    color: "#8d91a1"
                    wrapMode: Text.WordWrap
                }

                ColumnLayout {
                    objectName: "metricsOverlayControls"
                    Layout.fillWidth: true
                    Layout.minimumWidth: 0
                    spacing: 6

                    Label {
                        text: qsTr("Metrics sampling")
                        font.bold: true
                    }

                    RowLayout {
                        Layout.fillWidth: true
                        Layout.minimumWidth: 0
                        Label {
                            objectName: "metricsSamplingStatus"
                            Layout.minimumWidth: 0
                            text: root.runtime.samplingActive
                                  ? qsTr("Sampling is active")
                                  : qsTr("Sampling is inactive")
                            color: root.runtime.samplingActive
                                   ? "#4bd98d" : "#8d91a1"
                        }
                        Item { Layout.fillWidth: true }
                        Label {
                            objectName: "metricsSamplingExplanation"
                            Layout.fillWidth: true
                            Layout.minimumWidth: 0
                            Layout.maximumWidth: 360
                            text: qsTr("Metric selection is applied together with the display layout.")
                            color: "#8d91a1"
                            wrapMode: Text.WordWrap
                            horizontalAlignment: Text.AlignRight
                        }
                    }
                }

                Label {
                    objectName: "displayApplyBlockReason"
                    Layout.fillWidth: true
                    Layout.minimumWidth: 0
                    Layout.preferredWidth: 0
                    visible: root.hasUnsavedChanges &&
                             !root.canApplyChanges &&
                             root.applyBlockReason.length > 0
                    text: root.applyBlockReason
                    color: "#f0d27a"
                    wrapMode: Text.WordWrap
                    Accessible.role: Accessible.AlertMessage
                    Accessible.name: text
                }

                PrimaryButton {
                    objectName: "applyDisplayButton"
                    Layout.alignment: Qt.AlignRight
                    text: qsTr("Apply to display")
                    enabled: root.canApplyChanges
                    onClicked: root.applyChanges()
                }
            }
        }
            }
        }

        GroupBox {
            Layout.fillWidth: true
            title: qsTr("Screen controls")

            ColumnLayout {
                anchors.fill: parent
                spacing: 12

                RowLayout {
                    Layout.fillWidth: true
                    spacing: 12

                    Label {
                        Layout.preferredWidth: 92
                        text: qsTr("Brightness")
                    }
                    Slider {
                        objectName: "brightnessSlider"
                        Layout.preferredWidth: 320
                        Layout.maximumWidth: 380
                        from: 0
                        to: 100
                        stepSize: 1
                        value: root.brightnessDraft
                        enabled: root.displayBaselineAvailable() &&
                                 !root.runtime.operationBusy
                        onMoved:
                            root.brightnessDraft = Math.round(value)
                    }
                    Label {
                        Layout.preferredWidth: 34
                        text: root.brightnessDraft.toString()
                    }
                    Item { Layout.fillWidth: true }
                }

                RowLayout {
                    Layout.fillWidth: true
                    spacing: 12

                    Label {
                        Layout.preferredWidth: 92
                        text: qsTr("Backlight")
                    }
                    Label {
                        Layout.preferredWidth: 74
                        text: root.runtime.backlightEnabled
                              ? qsTr("On") : qsTr("Off")
                        color: root.runtime.backlightEnabled
                               ? "#66d18f" : "#9ca4ac"
                    }
                    Button {
                        text: root.runtime.backlightEnabled
                              ? qsTr("Turn display off")
                              : qsTr("Turn display on")
                        enabled: root.displayBaselineAvailable() &&
                                 !root.runtime.operationBusy
                        onClicked: root.runtime.setBacklight(
                            !root.runtime.backlightEnabled)
                    }
                    Item { Layout.fillWidth: true }
                }

                RowLayout {
                    Layout.fillWidth: true
                    spacing: 12

                    Label {
                        Layout.preferredWidth: 92
                        text: qsTr("Orientation")
                    }
                    CheckBox {
                        objectName: "mirrorDraftCheckBox"
                        text: qsTr("Mirror")
                        checked: root.mirrorDraft
                        enabled: root.confirmedSnapshotReady
                        onClicked: root.mirrorDraft = checked
                    }
                    CheckBox {
                        objectName: "waterfallDraftCheckBox"
                        text: qsTr("Waterfall")
                        checked: root.waterfallDraft
                        enabled: root.confirmedSnapshotReady
                        onClicked: root.waterfallDraft = checked
                    }
                    Item { Layout.fillWidth: true }
                }

                Label {
                    Layout.fillWidth: true
                    text: qsTr(
                        "Brightness and orientation drafts are included in Apply to display.")
                    color: "#9ca4ac"
                    wrapMode: Text.WordWrap
                }
            }
        }

        GroupBox {
            objectName: "recentOperationsGroup"
            Layout.fillWidth: true
            Layout.preferredHeight: Math.min(
                310, Math.max(115, operationList.count * 64 + 45))
            title: qsTr("Recent operations")

            ListView {
                id: operationList

                anchors.fill: parent
                clip: true
                model: root.runtime.operationModel
                delegate: ItemDelegate {
                    id: operationDelegate

                    required property string operationId
                    required property string subject
                    required property string operationState
                    required property string message
                    required property bool canRetry

                    width: ListView.view.width
                    height: 62
                    contentItem: RowLayout {
                        Label {
                            Layout.fillWidth: true
                            text: (operationDelegate.subject ||
                                   operationDelegate.operationId) +
                                  "\n" +
                                  operationDelegate.operationState +
                                  ": " + operationDelegate.message
                            elide: Text.ElideRight
                        }
                        Button {
                            text: qsTr("Retry")
                            visible: operationDelegate.canRetry
                            enabled: !root.runtime.operationBusy
                            onClicked:
                                root.runtime.retryOperation(
                                    operationDelegate.operationId)
                        }
                    }
                }
            }
        }

        Item {
            Layout.fillHeight: true
            Layout.minimumHeight: 20
        }
    }

    MediaFilePicker {
        id: mediaPicker
        editor: root.editor
    }

    MediaExportPicker {
        id: exportPicker
        workflow: root.deviceMedia
        homeFolder: root.editor.homeFolder
    }

    Connections {
        target: root.deviceMedia

        function onStateChanged() {
            if (root.deviceMedia.overwriteConfirmationPending) {
                if (!overwriteConfirmation.opened)
                    overwriteConfirmation.open()
            } else if (overwriteConfirmation.opened) {
                overwriteConfirmation.close()
            }
        }
    }

    Dialog {
        id: overwriteConfirmation

        objectName: "deviceMediaOverwriteConfirmation"
        title: qsTr("Replace exported file?")
        modal: true
        anchors.centerIn: parent
        width: Math.min(520, root.width - 48)
        standardButtons: Dialog.Yes | Dialog.Cancel

        contentItem: Label {
            text: qsTr("“%1” already exists. Replace it with the device copy?")
                  .arg(root.deviceMedia.overwriteFileName)
            color: "#f4f6f7"
            wrapMode: Text.WordWrap
        }

        onAccepted: root.deviceMedia.confirmOverwrite()
        onRejected: {
            if (root.deviceMedia.overwriteConfirmationPending)
                root.deviceMedia.cancelOverwrite()
        }
    }

    Dialog {
        id: deleteConfirmation

        objectName: "deleteConfirmation"
        title: qsTr("Delete media")
        modal: true
        anchors.centerIn: parent
        width: Math.min(520, root.width - 48)
        standardButtons: Dialog.Yes | Dialog.Cancel

        contentItem: Label {
            text: qsTr("Delete “%1” from the device? This cannot be undone.")
                  .arg(root.pendingDeleteName)
            color: "#f4f6f7"
            wrapMode: Text.WordWrap
        }

        onAccepted: {
            const target = root.pendingDeleteName
            if (target.length > 0) {
                root.runtime.deleteMedia([target])
                root.selectedMedia =
                    root.selectedMedia.filter(
                        name => name !== target)
            }
            root.pendingDeleteName = ""
        }
        onRejected: root.pendingDeleteName = ""
    }
}
