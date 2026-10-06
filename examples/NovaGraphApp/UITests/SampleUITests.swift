import XCTest

final class SampleUITests: XCTestCase {
    @MainActor func testEditPathsErrorPressureAndReopen() throws {
        continueAfterFailure = false
        let app = XCUIApplication()
        let location = NSTemporaryDirectory() + "nova-ui-" + UUID().uuidString
        app.launchArguments = ["--database", location, "-ApplePersistenceIgnoreState", "YES"]
        defer { app.terminate(); try? FileManager.default.removeItem(atPath: location) }
        app.launch()
        XCTAssertTrue(app.buttons["save-person"].waitForExistence(timeout: 10))
        waitEnabled(app.buttons["save-person"])
        set(app.textFields["person-id"], "ada"); set(app.textFields["person-name"], "Ada")
        press(app.buttons["save-person"]); waitStatus(app, "Saved")
        set(app.textFields["person-id"], "grace"); set(app.textFields["person-name"], "Grace")
        press(app.buttons["save-person"]); waitStatus(app, "Saved")
        set(app.textFields["edge-from"], "ada"); set(app.textFields["edge-to"], "grace")
        press(app.buttons["connect"]); waitStatus(app, "Connected")
        press(app.buttons["show-paths"]); waitStatus(app, "Found 1 paths")
        XCTAssertTrue(text(app, "ada → grace").exists)
        // macOS XCTest may capture an unrelated display on multi-monitor hosts.
        #if os(iOS)
        let screenshot = XCTAttachment(screenshot: app.windows.firstMatch.screenshot()); screenshot.name = "sample-tested-ui"; screenshot.lifetime = .keepAlways; add(screenshot)
        #endif
        press(app.buttons["query-tab"])
        set(app.textViews["query-editor"], "upsert node Person partial set name=\"No\"; delete node missing")
        press(app.buttons["run-query"]); waitStatus(app, "Operation failed")
        XCTAssertTrue(app.staticTexts["operation-error"].exists)
        press(app.buttons["lifecycle-tab"])
        press(app.buttons["trim"])
        XCTAssertTrue(app.staticTexts.containing(NSPredicate(format: "label CONTAINS 'Evicted' OR value CONTAINS 'Evicted'")).firstMatch.waitForExistence(timeout: 5))
        press(app.buttons["suspend"]); waitStatus(app, "Suspended")
        press(app.buttons["resume"]); waitStatus(app, "Ready")
        press(app.buttons["close"]); waitStatus(app, "Closed")
        app.terminate(); app.launch()
        waitEnabled(app.buttons["save-person"])
        XCTAssertTrue(text(app, "ada · Ada").exists)
        XCTAssertTrue(text(app, "grace · Grace").exists)
        XCTAssertFalse(text(app, "partial · No").exists)
        #if os(macOS)
        // Closing the last window must restore a visible error/recovery window.
        let current = URL(fileURLWithPath: location).appendingPathComponent("CURRENT")
        let saved = URL(fileURLWithPath: location).appendingPathComponent("saved-current")
        try FileManager.default.moveItem(at: current, to: saved)
        try FileManager.default.createDirectory(at: current, withIntermediateDirectories: false)
        app.typeKey("w", modifierFlags: .command)
        waitStatus(app, "Close failed")
        XCTAssertTrue(app.staticTexts["operation-error"].exists)
        try FileManager.default.removeItem(at: current)
        try FileManager.default.moveItem(at: saved, to: current)
        press(app.buttons["open"]); waitStatus(app, "Ready")
        XCTAssertTrue(text(app, "ada · Ada").exists)
        #endif
        press(app.buttons["close"]); waitStatus(app, "Closed")
    }

    @MainActor func testUnavailableStorageIsVisible() {
        continueAfterFailure = false
        let app = XCUIApplication()
        app.launchArguments = ["--database", "/dev/null/nova-sample-unavailable"]
        app.launch()
        XCTAssertTrue(app.staticTexts["operation-error"].waitForExistence(timeout: 10))
        waitStatus(app, "Operation failed")
        press(app.buttons["close"]); waitStatus(app, "Closed")
    }

    @MainActor private func press(_ element: XCUIElement) {
        #if os(macOS)
        XCUIApplication().activate()
        reveal(element)
        element.click()
        #else
        element.tap()
        #endif
    }
    #if os(macOS)
    @MainActor private func reveal(_ element: XCUIElement) {
        let scroll = XCUIApplication().scrollViews.firstMatch
        for _ in 0..<5 {
            if element.isHittable { return }
            scroll.scroll(byDeltaX: 0, deltaY: -160)
        }
    }
    #endif
    @MainActor private func text(_ app: XCUIApplication, _ value: String) -> XCUIElement {
        app.staticTexts.matching(NSPredicate(format: "label == %@ OR value == %@", value, value)).firstMatch
    }
    @MainActor private func waitStatus(_ app: XCUIApplication, _ value: String) {
        let predicate = NSPredicate(format: "label == %@ OR value == %@", value, value)
        expectation(for: predicate, evaluatedWith: app.staticTexts["operation-status"])
        waitForExpectations(timeout: 10)
    }
    @MainActor private func waitEnabled(_ element: XCUIElement) {
        expectation(for: NSPredicate(format: "exists == true AND enabled == true"), evaluatedWith: element)
        waitForExpectations(timeout: 10)
    }
    @MainActor private func set(_ field: XCUIElement, _ text: String) {
        #if os(macOS)
        XCUIApplication().activate()
        reveal(field)
        field.click()
        field.typeKey("a", modifierFlags: .command)
        field.typeText(text)
        #else
        field.tap()
        let old = field.value as? String ?? ""
        field.typeText(String(repeating: XCUIKeyboardKey.delete.rawValue, count: old.count) + text)
        #endif
    }
}
