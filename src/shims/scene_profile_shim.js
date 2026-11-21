/*
 * Copyright (c) 2026 General Arcade (Pte. Ltd.)
 * SPDX-License-Identifier: GPL-3.0-or-later OR LicenseRef-GeneralArcade-Commercial
 */

(function() {
    "use strict";

    if (globalThis.__outsiderProfileScenes !== true ||
        typeof SceneManager === "undefined") {
        return;
    }

    var manager = SceneManager;
    var activeTransition = null;

    function now() {
        return (typeof performance !== "undefined" && performance.now)
            ? performance.now()
            : Date.now();
    }

    function sceneName(sceneOrClass) {
        if (!sceneOrClass) return "none";
        var ctor = typeof sceneOrClass === "function"
            ? sceneOrClass
            : sceneOrClass.constructor;
        return ctor.name || "unknown";
    }

    function formatMs(value) {
        return value.toFixed(1);
    }

    var originalGoto = manager.goto;
    manager.goto = function(sceneClass) {
        activeTransition = sceneClass ? {
            requestedAt: now(),
            from: sceneName(this._scene),
            to: sceneName(sceneClass)
        } : null;
        return originalGoto.apply(this, arguments);
    };

    var originalChangeScene = manager.changeScene;
    manager.changeScene = function() {
        var previousScene = this._scene;
        var changeStartedAt = now();
        var result = originalChangeScene.apply(this, arguments);

        if (this._scene && this._scene !== previousScene) {
            var to = sceneName(this._scene);
            if (!activeTransition || activeTransition.to !== to) {
                activeTransition = {
                    requestedAt: changeStartedAt,
                    from: sceneName(previousScene),
                    to: to
                };
            }
            activeTransition.scene = this._scene;
            activeTransition.queuedMs = changeStartedAt - activeTransition.requestedAt;
            activeTransition.changeMs = now() - changeStartedAt;
            activeTransition.changedAt = now();
            activeTransition.readyWorkMs = 0;
            activeTransition.readyPolls = 0;
        }

        return result;
    };

    var originalUpdateScene = manager.updateScene;
    manager.updateScene = function() {
        var scene = this._scene;
        var transition = activeTransition;
        var waitingForStart = transition && transition.scene === scene &&
            scene && !scene.isStarted();
        var updateStartedAt = now();
        var result = originalUpdateScene.apply(this, arguments);
        var updateMs = now() - updateStartedAt;

        if (waitingForStart) {
            if (scene.isStarted()) {
                var readyMs = now() - transition.changedAt;
                var totalMs = now() - transition.requestedAt;
                __native_io.logInfo(
                    "[scene] " + transition.from + " -> " + transition.to +
                    " queued=" + formatMs(transition.queuedMs) + "ms" +
                    " change=" + formatMs(transition.changeMs) + "ms" +
                    " ready=" + formatMs(readyMs) + "ms" +
                    " ready-work=" + formatMs(transition.readyWorkMs) +
                    "ms/" + transition.readyPolls +
                    " start-call=" + formatMs(updateMs) + "ms" +
                    " total=" + formatMs(totalMs) + "ms"
                );
                if (activeTransition === transition) activeTransition = null;
            } else {
                transition.readyWorkMs += updateMs;
                transition.readyPolls++;
            }
        }

        return result;
    };
})();
