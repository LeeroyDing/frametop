#!/usr/bin/env swift
// Copyright (c) 2026 Leeroy Ding. SPDX-License-Identifier: MIT
//
// ft-apple-vdisplay.swift — Mount a local macOS CGVirtualDisplay for Frametop's
// Apple High Performance Screen Sharing `apple:physical` mode.
//
// Why this exists:
//   Apple's built-in Screen Sharing virtual display (`0x1d` / `apple:virtual`)
//   unconditionally starts `VirtualDisplayHIDFilter` inside `screensharingd`,
//   dropping all local Mac keyboard, mouse, and Magic Trackpad events.
//   Mounting the virtual display locally on the Mac via `CGVirtualDisplay` and
//   connecting Frametop with `apple:physical` streams the same 60 fps hardware
//   H.264 feed (`CGMainDisplayID()`) while keeping the Mac's attached Magic
//   Trackpad, keyboard, and mouse 100% native inside macOS.
//
// Usage:
//   swift host/macos/ft-apple-vdisplay.swift [WxH[@HZ]] [--hidpi] [--no-main] [--name NAME]
//   # or compile once for instant startup:
//   swiftc -O host/macos/ft-apple-vdisplay.swift -o ~/.local/bin/ft-apple-vdisplay

import CoreGraphics
import Foundation
import ObjectiveC.runtime

struct Config {
    var width: UInt32 = 3456
    var height: UInt32 = 972
    var refreshHz: Double = 60.0
    var hiDPI: Bool = false
    var makeMain: Bool = true
    var dryRun: Bool = false
    var name: String = "Frametop Virtual Display"
    var serial: UInt32 = 1
}

func printUsage() {
    print("""
    usage: ft-apple-vdisplay [WxH[@HZ]] [--hidpi] [--no-main] [--name TEXT] [--serial N] [--dry-run]

    Mounts a local CoreGraphics virtual display (default: 3456x972@60 1x) until interrupted (Ctrl+C).
    By default, promotes the virtual display to Main Display for the current session so macOS Screen
    Sharing (`apple:physical`) captures it while keeping local Magic Trackpad & keyboard gestures live.
    """)
}

func parseSizeSpec(_ spec: String, into cfg: inout Config) -> Bool {
    let parts = spec.split(separator: "@", maxSplits: 1, omittingEmptySubsequences: false)
    let dims = parts[0].lowercased().split(separator: "x", maxSplits: 1, omittingEmptySubsequences: false)
    guard dims.count == 2,
          let w = UInt32(dims[0]), let h = UInt32(dims[1]),
          w >= 640, w <= 7680, h >= 480, h <= 4320 else {
        return false
    }
    if parts.count == 2 {
        guard let hz = Double(parts[1]), hz >= 10.0, hz <= 240.0 else { return false }
        cfg.refreshHz = hz
    }
    cfg.width = w
    cfg.height = h
    return true
}

func parseArgs() -> Config {
    var cfg = Config()
    let args = Array(CommandLine.arguments.dropFirst())
    var i = 0
    while i < args.count {
        let a = args[i]
        switch a {
        case "-h", "--help":
            printUsage()
            exit(0)
        case "--hidpi":
            cfg.hiDPI = true
        case "--no-main":
            cfg.makeMain = false
        case "--dry-run":
            cfg.dryRun = true
        case "--name":
            guard i + 1 < args.count else {
                fputs("ft-apple-vdisplay: --name requires a value\n", stderr)
                exit(2)
            }
            cfg.name = args[i + 1]
            i += 1
        case "--serial":
            guard i + 1 < args.count, let s = UInt32(args[i + 1]), s > 0 else {
                fputs("ft-apple-vdisplay: --serial requires a positive integer\n", stderr)
                exit(2)
            }
            cfg.serial = s
            i += 1
        default:
            if a.hasPrefix("-") || !parseSizeSpec(a, into: &cfg) {
                fputs("ft-apple-vdisplay: invalid argument '\(a)'\n", stderr)
                printUsage()
                exit(2)
            }
        }
        i += 1
    }
    return cfg
}

func makeNSObject(_ className: String) -> NSObject? {
    guard let cls = NSClassFromString(className) as? NSObject.Type else { return nil }
    return cls.init()
}

func makeMode(width: UInt32, height: UInt32, refreshRate: Double) -> AnyObject? {
    guard let cls: AnyClass = NSClassFromString("CGVirtualDisplayMode"),
          let raw = class_createInstance(cls, 0) as AnyObject? else { return nil }
    let sel = sel_registerName("initWithWidth:height:refreshRate:")
    guard let m = class_getInstanceMethod(cls, sel) else { return nil }
    typealias InitFn = @convention(c) (AnyObject, Selector, UInt32, UInt32, Double) -> Unmanaged<AnyObject>?
    let fn = unsafeBitCast(method_getImplementation(m), to: InitFn.self)
    let retainedSelf = Unmanaged.passRetained(raw)
    return fn(retainedSelf.takeUnretainedValue(), sel, width, height, refreshRate)?.takeRetainedValue()
}

func setDispatchQueue(_ target: AnyObject, queue: DispatchQueue) -> Bool {
    let sel = sel_registerName("setDispatchQueue:")
    guard let cls: AnyClass = object_getClass(target),
          let m = class_getInstanceMethod(cls, sel) else { return false }
    typealias SetQueueFn = @convention(c) (AnyObject, Selector, DispatchQueue) -> Void
    let fn = unsafeBitCast(method_getImplementation(m), to: SetQueueFn.self)
    fn(target, sel, queue)
    return true
}

func createVirtualDisplay(descriptor: AnyObject, settings: AnyObject) -> (display: AnyObject, id: CGDirectDisplayID)? {
    guard let cls: AnyClass = NSClassFromString("CGVirtualDisplay"),
          let raw = class_createInstance(cls, 0) as AnyObject? else { return nil }
    let initSel = sel_registerName("initWithDescriptor:")
    let applySel = sel_registerName("applySettings:")
    let idSel = sel_registerName("displayID")
    guard let initM = class_getInstanceMethod(cls, initSel),
          let applyM = class_getInstanceMethod(cls, applySel),
          let idM = class_getInstanceMethod(cls, idSel) else { return nil }

    typealias InitFn = @convention(c) (AnyObject, Selector, AnyObject) -> Unmanaged<AnyObject>?
    typealias ApplyFn = @convention(c) (AnyObject, Selector, AnyObject) -> Bool
    typealias IdFn = @convention(c) (AnyObject, Selector) -> CGDirectDisplayID

    let initFn = unsafeBitCast(method_getImplementation(initM), to: InitFn.self)
    let applyFn = unsafeBitCast(method_getImplementation(applyM), to: ApplyFn.self)
    let idFn = unsafeBitCast(method_getImplementation(idM), to: IdFn.self)

    let retainedSelf = Unmanaged.passRetained(raw)
    guard let disp = initFn(retainedSelf.takeUnretainedValue(), initSel, descriptor)?.takeRetainedValue() else {
        return nil
    }
    guard applyFn(disp, applySel, settings) else { return nil }
    let did = idFn(disp, idSel)
    return did != 0 ? (disp, did) : nil
}

func promoteToMainDisplay(virtualID: CGDirectDisplayID, logicalWidth: UInt32, logicalHeight: UInt32) {
    let oldMain = CGMainDisplayID()
    if oldMain == virtualID || oldMain == 0 { return }

    var active = [CGDirectDisplayID](repeating: 0, count: 16)
    var count: UInt32 = 0
    guard CGGetActiveDisplayList(UInt32(active.count), &active, &count) == .success, count > 1 else { return }

    let oldMainBounds = CGDisplayBounds(oldMain)
    let dx = Int32((Int(logicalWidth) - Int(oldMainBounds.width)) / 2)
    let dy = Int32(logicalHeight)

    var configRef: CGDisplayConfigRef?
    guard CGBeginDisplayConfiguration(&configRef) == .success, let cfg = configRef else { return }

    CGConfigureDisplayOrigin(cfg, virtualID, 0, 0)
    for i in 0..<Int(count) {
        let d = active[i]
        if d == virtualID || CGDisplayMirrorsDisplay(d) != kCGNullDirectDisplay { continue }
        let b = CGDisplayBounds(d)
        CGConfigureDisplayOrigin(cfg, d, Int32(b.origin.x) + dx, Int32(b.origin.y) + dy)
    }
    let err = CGCompleteDisplayConfiguration(cfg, .forSession)
    if err != .success {
        CGCancelDisplayConfiguration(cfg)
        fputs("ft-apple-vdisplay: warning: CGCompleteDisplayConfiguration returned \(err.rawValue)\n", stderr)
    }
}

let cfg = parseArgs()
let maxW = cfg.hiDPI ? cfg.width * 2 : cfg.width
let maxH = cfg.hiDPI ? cfg.height * 2 : cfg.height
// ~92 PPI physical size hint in millimetres so macOS font metrics stay crisp.
let mmW = Double(cfg.width) * 25.4 / 92.0
let mmH = Double(cfg.height) * 25.4 / 92.0

guard let desc = makeNSObject("CGVirtualDisplayDescriptor"),
      let settings = makeNSObject("CGVirtualDisplaySettings"),
      let primaryMode = makeMode(width: cfg.width, height: cfg.height, refreshRate: cfg.refreshHz) else {
    fputs("ft-apple-vdisplay: CoreGraphics CGVirtualDisplay API unavailable on this macOS build\n", stderr)
    exit(1)
}

var modes: [AnyObject] = [primaryMode]
if cfg.hiDPI, let backingMode = makeMode(width: maxW, height: maxH, refreshRate: cfg.refreshHz) {
    modes.append(backingMode)
}

desc.setValue(cfg.name, forKey: "name")
desc.setValue(0x4654, forKey: "vendorID")
desc.setValue(0x3239, forKey: "productID")
desc.setValue(cfg.serial, forKey: "serialNum")
desc.setValue(maxW, forKey: "maxPixelsWide")
desc.setValue(maxH, forKey: "maxPixelsHigh")
desc.setValue(NSValue(size: NSSize(width: mmW, height: mmH)), forKey: "sizeInMillimeters")
desc.setValue(NSValue(point: NSPoint(x: 0.680, y: 0.320)), forKey: "redPrimary")
desc.setValue(NSValue(point: NSPoint(x: 0.265, y: 0.690)), forKey: "greenPrimary")
desc.setValue(NSValue(point: NSPoint(x: 0.150, y: 0.060)), forKey: "bluePrimary")
desc.setValue(NSValue(point: NSPoint(x: 0.3127, y: 0.3290)), forKey: "whitePoint")
_ = setDispatchQueue(desc, queue: DispatchQueue.main)

settings.setValue(cfg.hiDPI ? 1 : 0, forKey: "hiDPI")
settings.setValue(modes, forKey: "modes")

if cfg.dryRun {
    print("dry-run ok: \(cfg.name) logical=\(cfg.width)x\(cfg.height)@\(Int(cfg.refreshHz)) max=\(maxW)x\(maxH) hidpi=\(cfg.hiDPI ? "on" : "off") main=\(cfg.makeMain ? "on" : "off")")
    exit(0)
}

var activeDisplay: AnyObject?
var displayID: CGDirectDisplayID = 0
autoreleasepool {
    guard let mounted = createVirtualDisplay(descriptor: desc, settings: settings) else {
        fputs("ft-apple-vdisplay: failed to mount CGVirtualDisplay (\(cfg.width)x\(cfg.height)@\(Int(cfg.refreshHz)))\n", stderr)
        exit(1)
    }
    activeDisplay = mounted.display
    displayID = mounted.id
}

if cfg.makeMain {
    // Allow WindowServer a brief beat to register the new CGDirectDisplayID before promoting it.
    usleep(200_000)
    promoteToMainDisplay(virtualID: displayID, logicalWidth: cfg.width, logicalHeight: cfg.height)
}

setbuf(stdout, nil)
print("Mounted '\(cfg.name)' (displayID=\(displayID), \(cfg.width)x\(cfg.height)@\(Int(cfg.refreshHz))\(cfg.hiDPI ? " HiDPI" : ""), main=\(CGMainDisplayID() == displayID ? "yes" : "no")).")
print("Press Ctrl+C to unmount and restore your previous display arrangement.")

signal(SIGINT, SIG_IGN)
signal(SIGTERM, SIG_IGN)
let sigInt = DispatchSource.makeSignalSource(signal: SIGINT, queue: .main)
let sigTerm = DispatchSource.makeSignalSource(signal: SIGTERM, queue: .main)
let shutdown: () -> Void = {
    autoreleasepool {
        activeDisplay = nil
    }
    print("\nUnmounted '\(cfg.name)' (displayID=\(displayID)).")
    CFRunLoopStop(CFRunLoopGetMain())
}
sigInt.setEventHandler(handler: shutdown)
sigTerm.setEventHandler(handler: shutdown)
sigInt.resume()
sigTerm.resume()

CFRunLoopRun()
