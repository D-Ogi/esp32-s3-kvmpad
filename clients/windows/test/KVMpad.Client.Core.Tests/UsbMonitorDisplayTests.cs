// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// Copyright 2026 D-Ogi. Noncommercial use only; see LICENSE.md.
// Required Notice: Copyright 2026 D-Ogi (https://github.com/D-Ogi/esp32-s3-kvmpad)

using System;
using System.Collections.Generic;
using System.Linq;
using System.Runtime.InteropServices;
using System.Threading.Tasks;
using KVMpad.Client.Core.Display;
using Xunit;

namespace KVMpad.Client.Core.Tests;

public sealed class UsbMonitorDisplayTests
{
    private static readonly DisplayResolution Vga = new(640, 480), Wide = new(1024, 600);
    private const string Usb = @"USB\VID_303A&PID_2987\ABCDEF012345";
    private const string Composite = @"USB\VID_303A&PID_2986\ABCDEF012345";
    private static readonly DisplayPath Pc = new(new(1, 0, 0), new(1, 0, 1), @"\\.\DISPLAY1", null, 0, 0,
        new(1920, 1080), new(1920, 1080), 1, 1, 60000, 1000, 5, 1);
    private static readonly DisplayPath Board = Pc with { Source = new(9, 1, 0), Target = new(9, 1, 1),
        Name = @"\\.\DISPLAY4", UsbId = Usb, X = 1920, Size = Vga, Signal = Wide };

    private sealed class Api : IDisplayApi
    {
        public DisplayPath[] Paths = [Pc, Board];
        public SourceMode Mode = new(Vga, 60, 0, 32, 0);
        public DisplayResolution[]? OfferedSizes;
        public Action? WhenOffered, AfterTest, AfterApply;
        public List<(string Name, SourceMode Request, bool Test, DisplayPosition? Position)> Calls = [];
        public DisplayPath[] ActivePaths() => [..Paths];
        public SourceMode Current(string name) => Mode;
        public IEnumerable<SourceMode> Offered(string name)
        {
            WhenOffered?.Invoke();
            if (OfferedSizes != null) return OfferedSizes.Select(size => Mode with { Size = size });
            return [Mode with {Size=Vga}, Mode with {Size=Wide}, Mode with {Size=Wide},
                Mode with {Size=new(800, 600), Hertz=59}, Mode with {Size=new(600, 1024), Orientation=1},
                Mode with {Size=new(800, 600), Bits=16}, Mode with {Size=new(800, 600), Flags=2}];
        }
        public void Change(string name, SourceMode mode, bool test, DisplayPosition? position)
        {
            Calls.Add((name, mode, test, position));
            int index = Array.FindIndex(Paths, p => p.Name == name);
            Assert.True(index >= 0);
            if (test) { AfterTest?.Invoke(); return; }
            Mode = mode;
            var path = Paths[index];
            Paths[index] = path with {Size=mode.Size, X=position?.X ?? path.X, Y=position?.Y ?? path.Y};
            AfterApply?.Invoke();
        }
    }

    [Fact]
    public void PhysicalIdentityNeedsCorrectUsbFunctionAndSerial()
    {
        static string? Match(params string[] ids) => UsbMonitorDisplayService.BoardIdentity(ids);
        Assert.Equal(Usb, Match(@"DISPLAY\ABC\123", Usb.ToLowerInvariant()));
        Assert.Equal(Composite, Match(@"SWD\Foo\123", @"USB\VID_303A&PID_2986&MI_00\7&ABC", Composite));
        Assert.Null(Match(@"USB\VID_303A&PID_2986&MI_01\7&ABC", Composite));
        Assert.Null(Match(Composite));
        Assert.Null(Match(Usb.Replace("F012345", "X012345")));
        Assert.Null(Match(Usb + "0"));
        Assert.Null(Match(Usb.Replace("303A", "303B")));
        Assert.Null(Match(@"USB\VID_303A&PID_2986&MI_00\7&ABC", Usb));
        Assert.Null(Match(@"DISPLAY\VID_303A&PID_2987\ABCDEF012345"));
    }

    [Fact]
    public void ShowsOnlyCompatibleModesAndChangesJustTheVerifiedDevice()
    {
        var api = new Api();
        var service = new UsbMonitorDisplayService(api);
        var first = service.Read();
        Assert.Equal("ABCDEF012345", first.Serial);
        Assert.Equal(Board.Name, first.SourceName);
        Assert.Equal(Wide, first.Signal);
        Assert.Equal(Vga, first.Current);
        Assert.Equal(new[] {Vga, Wide}, first.Modes);
        Assert.Empty(api.Calls);
        Assert.Equal(Vga, service.Apply(first, Vga).Current);
        Assert.Empty(api.Calls);
        var second = service.Apply(first, Wide);
        Assert.Equal(Wide, second.Current);
        Assert.Equal(new[] {true, false}, api.Calls.Select(c => c.Test));
        Assert.All(api.Calls, c => {
            Assert.Equal(Board.Name, c.Name);
            Assert.Equal(new SourceMode(Wide, 60, 0, 32, 0), c.Request);
            Assert.Null(c.Position);
        });
        Assert.Equal(Pc, api.Paths[0]);
        Assert.Throws<InvalidOperationException>(() => service.Apply(first, Vga));
        Assert.Equal(2, api.Calls.Count);
    }

    [Fact]
    public void MatchSignalRequiresTheSelectedBoardAndReportedWireMode()
    {
        var api = new Api();
        var service = new UsbMonitorDisplayService(api);
        Assert.Throws<InvalidOperationException>(() => service.MatchSignal("0123456789AB", Wide));
        Assert.Throws<InvalidOperationException>(() => service.MatchSignal("ABCDEF012345", Vga));
        api.Paths[1] = Board with { Signal = null };
        Assert.Throws<InvalidOperationException>(() => service.MatchSignal("ABCDEF012345", Wide));
        Assert.Empty(api.Calls);
        api.Paths[1] = Board;
        Assert.Equal(Wide, service.MatchSignal("abcdef012345", Wide).Current);
        Assert.Equal(Wide, service.MatchSignal("abcdef012345", Wide).Signal);
        Assert.Equal(2, api.Calls.Count);
        Assert.Equal(Pc, api.Paths[0]);
        var raced = new Api();
        raced.AfterApply = () => raced.Paths[1] = raced.Paths[1] with { Signal = Vga };
        Assert.Throws<InvalidOperationException>(() => new UsbMonitorDisplayService(raced).MatchSignal("ABCDEF012345", Wide));
    }

    [Fact]
    public void ChoicesExcludeUnmatchableActiveSignalButRetainPairedProfileSwitch()
    {
        var desktop = new DisplayResolution(800, 600);
        var api = new Api { Paths = [Pc, Board with { Size = desktop }], OfferedSizes = [desktop] };
        api.Mode = api.Mode with { Size = desktop };
        var display = new UsbMonitorDisplayService(api).Read();
        Assert.Equal(Wide, display.Signal);
        Assert.Equal(new[] {desktop}, UsbMonitorDisplayService.Choices(display, false));
        Assert.Equal(new[] {Vga, desktop}, UsbMonitorDisplayService.Choices(display, true));
        Assert.Empty(api.Calls);
    }

    private static void AssertSignalAndDesktop(Exception error)
    {
        Assert.Contains(Wide.ToString(), error.Message);
        Assert.Contains(Vga.ToString(), error.Message);
    }

    [Theory]
    [InlineData(false)]
    [InlineData(true)]
    public async Task MatchingAnAlreadyActiveSignalWithoutACompatibleDesktopModeDoesNotReconnect(bool noOffers)
    {
        var api = new Api { OfferedSizes = noOffers ? [] : [Vga] };
        var service = new UsbMonitorDisplayService(api);
        var display = service.Read();
        Assert.Equal(Vga, display.Current);
        Assert.Equal(Wide, display.Signal);
        if (noOffers) Assert.Empty(display.Modes);
        AssertSignalAndDesktop(Assert.Throws<InvalidOperationException>(() => service.MatchSignal(display.Serial, Wide)));
        int requests = 0;
        var error = await Assert.ThrowsAsync<InvalidOperationException>(() => service.ApplyProfileAsync(display, Wide,
            (_, _, _) => { requests++; return Task.CompletedTask; }, default));
        AssertSignalAndDesktop(error);
        Assert.Equal(0, requests);
        Assert.Empty(api.Calls);
    }

    [Fact]
    public async Task MatchingCurrentDesktopAndSignalNeedsNoEnumeratedMode()
    {
        var api = new Api { Paths = [Pc, Board with { Signal = Vga }], OfferedSizes = [] };
        var service = new UsbMonitorDisplayService(api);
        var display = service.Read();
        Assert.Empty(display.Modes);
        Assert.Empty(UsbMonitorDisplayService.Choices(display, false));
        Assert.Equal(new[] {Wide}, UsbMonitorDisplayService.Choices(display, true));
        Assert.Equal(Vga, service.MatchSignal(display.Serial, Vga).Current);
        var result = await service.ApplyProfileAsync(display, Vga,
            (_, _, _) => throw new InvalidOperationException("Already in VGA; unexpected USB reconnect."), default);
        Assert.Equal(Vga, result.Current);
        Assert.Equal(Vga, result.Signal);
        Assert.Empty(api.Calls);
    }

    [Fact]
    public async Task ConfirmedSignalWaitsForWindowsToOfferTheMatchingDesktopSize()
    {
        var api = new Api { Paths = [Pc, Board with { Signal = Vga }], OfferedSizes = [Vga] };
        var service = new UsbMonitorDisplayService(api);
        int polls = 0, requests = 0;
        var result = await service.ApplyProfileAsync(service.Read(), Wide, (serial, native, _) => {
            Assert.Equal("ABCDEF012345", serial);
            Assert.True(native);
            requests++;
            api.Paths[1] = Board;
            api.WhenOffered = () => {
                if (++polls == 3) {
                    Assert.Empty(api.Calls);
                    api.OfferedSizes = [Vga, Wide];
                }
            };
            return Task.CompletedTask;
        }, default);
        Assert.True(polls >= 3);
        Assert.Equal(1, requests);
        Assert.Equal(Wide, result.Signal);
        Assert.Equal(Wide, result.Current);
        Assert.Equal(new[] {true, false}, api.Calls.Select(c => c.Test));
        Assert.All(api.Calls, c => Assert.Equal(Wide, c.Request.Size));
    }

    [Fact]
    public async Task ConfirmedSignalWithPersistentlyMissingWindowsModeReportsTheDesktopSize()
    {
        var api = new Api { Paths = [Pc, Board with { Signal = Vga }], OfferedSizes = [Vga] };
        var service = new UsbMonitorDisplayService(api);
        int requests = 0;
        var error = await Assert.ThrowsAsync<InvalidOperationException>(() => service.ApplyProfileAsync(service.Read(), Wide,
            (_, _, _) => {
                requests++;
                api.Paths[1] = Board;
                return Task.CompletedTask;
            }, default));
        AssertSignalAndDesktop(error);
        Assert.Equal(1, requests);
        Assert.Equal(Wide, api.Paths[1].Signal);
        Assert.Equal(Vga, api.Mode.Size);
        Assert.Empty(api.Calls);
    }

    [Fact]
    public void RefusesMissingClonedOverlappingAndAmbiguousDevices()
    {
        DisplayPath[][] invalid = [[Pc], [Board], [Pc, Board with {Source=Pc.Source}],
            [Pc, Board with {X=100}], [Pc, Board, Board with {Target=new(9,1,2)}]];
        foreach (var paths in invalid)
        {
            var api = new Api { Paths = paths };
            Assert.Throws<InvalidOperationException>(() => new UsbMonitorDisplayService(api).Read());
            Assert.Empty(api.Calls);
        }
    }

    [Fact]
    public void RefusesStaleOrUnsafeRequestsBeforeApplication()
    {
        var api = new Api();
        var service = new UsbMonitorDisplayService(api);
        var observed = service.Read();
        Assert.Throws<InvalidOperationException>(() => service.Apply(observed, new(800, 600)));
        api.Paths[1] = Board with {UsbId=Composite};
        Assert.Throws<InvalidOperationException>(() => service.Apply(observed, Wide));
        Assert.Empty(api.Calls);
        api.Paths[1] = Board;
        api.AfterTest = () => api.Paths[0] = Pc with {Rotation=3};
        Assert.Throws<InvalidOperationException>(() => service.Apply(observed, Wide));
        Assert.True(Assert.Single(api.Calls).Test);
    }

    [Theory]
    [InlineData(-640, 120, -1024, 120)] // USB right edge stays on the PC's left edge.
    [InlineData(240, -480, 240, -600)]  // USB bottom edge stays on the PC's top edge.
    public void AnchoredGrowthAndShrinkKeepTheSharedEdge(int x, int y, int wideX, int wideY)
    {
        var board = Board with {X=x, Y=y};
        var api = new Api {Paths=[Pc, board]};
        var service = new UsbMonitorDisplayService(api);
        var large = service.Apply(service.Read(), Wide);
        Assert.Equal(board with {X=wideX, Y=wideY, Size=Wide}, large.Board);
        Assert.Equal(large.Board, api.Paths[1]);
        var small = service.Apply(large, Vga);
        Assert.Equal(board, small.Board);
        Assert.Equal(small.Board, api.Paths[1]);
        Assert.Equal(Pc, api.Paths[0]);
        Assert.Equal(new[] {true, false, true, false}, api.Calls.Select(c => c.Test));
        for (int i = 0; i < api.Calls.Count; ++i)
        {
            var call = api.Calls[i];
            Assert.Equal(board.Name, call.Name);
            Assert.Equal(new SourceMode(i < 2 ? Wide : Vga, 60, 0, 32, 0), call.Request);
            var expected = i < 2 ? new DisplayPosition(wideX, wideY) : new DisplayPosition(x, y);
            Assert.Equal<DisplayPosition?>(expected, call.Position);
        }
    }

    [Fact]
    public void CanResizeAtTheDesktopOriginWhenNoMovementIsNeeded()
    {
        var peer = Pc with {X=-Pc.Size.Width};
        var board = Board with {X=0};
        var api = new Api {Paths=[peer, board]};
        var service = new UsbMonitorDisplayService(api);
        var wide = service.Apply(service.Read(), Wide);
        Assert.Equal(board with {Size=Wide}, wide.Board);
        Assert.Equal(peer, api.Paths[0]);
        Assert.Equal(new[] {true, false}, api.Calls.Select(c => c.Test));
        Assert.All(api.Calls, c => Assert.Null(c.Position));
    }

    [Fact]
    public void CannotMoveTheOriginDisplayOrBreakAnExistingChain()
    {
        var farLeft = Pc with {Source=new(2, 0, 0), Target=new(2, 0, 1), Name=@"\\.\DISPLAY2", X=-2560};
        DisplayPath[][] layouts = [
            [Pc with {X=640}, Board with {X=0}], // KVMpad is the primary at 0,0.
            [Pc, Board with {X=-640}, farLeft]   // No room for 1024 between the two PCs.
        ];
        foreach (var paths in layouts)
        {
            var api = new Api {Paths=[..paths]};
            var service = new UsbMonitorDisplayService(api);
            Assert.Throws<InvalidOperationException>(() => service.Apply(service.Read(), Wide));
            Assert.Empty(api.Calls);
            Assert.Equal(paths, api.Paths);
        }
    }

    [Fact]
    public void DiscardsAnAnchorIfTopologyChangesBeforeApply()
    {
        var left = Board with {X=-640, Y=120};
        var api = new Api {Paths=[Pc, left]};
        var service = new UsbMonitorDisplayService(api);
        var observed = service.Read();
        api.Paths[1] = left with {Y=121};
        Assert.Throws<InvalidOperationException>(() => service.Apply(observed, Wide));
        Assert.Empty(api.Calls);
        api.Paths[1] = left;
        api.AfterTest = () => api.Paths[0] = Pc with {Name=@"\\.\DISPLAY7"};
        Assert.Throws<InvalidOperationException>(() => service.Apply(observed, Wide));
        Assert.True(Assert.Single(api.Calls).Test);
        Assert.Equal(left, api.Paths[1]);
        Assert.Equal(Vga, api.Mode.Size);
    }

    [Theory]
    [InlineData(false, false)]
    [InlineData(true, false)]
    [InlineData(false, true)]
    public void ConfirmsTheObservedCoordinatesAndOtherDisplaysAfterApplying(bool above, bool peerChanged)
    {
        var board = above ? Board with {X=240, Y=-480} : Board with {X=-640, Y=120};
        var api = new Api {Paths=[Pc, board]};
        var service = new UsbMonitorDisplayService(api);
        api.AfterApply = () => {
            // Windows can report success and still slide a display one pixel along the shared edge.
            if (peerChanged) api.Paths[0] = Pc with {Y=1};
            else if (above) api.Paths[1] = api.Paths[1] with {X=board.X+1};
            else api.Paths[1] = api.Paths[1] with {Y=board.Y+1};
        };
        Assert.Throws<InvalidOperationException>(() => service.Apply(service.Read(), Wide));
        Assert.Equal(new[] {true, false}, api.Calls.Select(c => c.Test));
        Assert.Equal(Wide, api.Mode.Size);
    }

    [Fact]
    public void RechecksEnumerationTestFailureAndObservedResult()
    {
        var api = new Api();
        var service = new UsbMonitorDisplayService(api);
        api.WhenOffered = () => api.Paths[1] = Board with {UsbId=null};
        Assert.Throws<InvalidOperationException>(() => service.Read());
        api.WhenOffered = null; api.Paths[1] = Board;
        var first = service.Read();
        api.AfterTest = () => throw new InvalidOperationException("mode rejected");
        Assert.Throws<InvalidOperationException>(() => service.Apply(first, Wide));
        Assert.True(Assert.Single(api.Calls).Test);
        api.AfterTest = null;
        api.AfterApply = () => api.Paths[0] = Pc with {X=-5};
        Assert.Throws<InvalidOperationException>(() => service.Apply(first, Wide));
        Assert.Equal(1, api.Calls.Count(c => !c.Test));
    }

    [Theory]
    [InlineData(1920)]
    [InlineData(-640)]
    public async Task ProfileChangeFollowsSameUsbDeviceAcrossWindowsSourceRename(int x)
    {
        var api = new Api();
        var board = Board with {X=x, Signal=Vga};
        api.Paths[1] = board;
        var service = new UsbMonitorDisplayService(api);
        var original = service.Read();
        var result = await service.ApplyProfileAsync(original, Wide, (serial, native, token) => {
            Assert.Equal(original.Serial, serial);
            Assert.True(native);
            api.Paths[1] = board with {Name=@"\\.\DISPLAY49", Target=new(13,0,1), Signal=Wide};
            return Task.CompletedTask;
        }, default);
        Assert.Equal(Wide, result.Current);
        Assert.Equal(Wide, result.Signal);
        Assert.Equal(x < 0 ? -1024 : x, result.Board.X);
        Assert.Equal(Pc, api.Paths[0]);
        Assert.All(api.Calls, call => Assert.Equal(result.SourceName, call.Name));
        Assert.NotEqual(original.SourceName, result.SourceName);
    }

    [Fact]
    public async Task ProfileChangeRejectsReplacementBoardBeforeApplyingDesktopMode()
    {
        var api = new Api();
        api.Paths[1] = Board with {Signal=Vga};
        var service = new UsbMonitorDisplayService(api);
        await Assert.ThrowsAsync<InvalidOperationException>(() => service.ApplyProfileAsync(service.Read(), Wide,
            (_,_,_) => { api.Paths[1] = Board with {UsbId=Usb.Replace("ABC", "DEF")}; return Task.CompletedTask; }, default));
        Assert.Empty(api.Calls);
    }

    [Fact]
    public void NativePacketsMatchWindowsSdkAndIncludePositionOnlyWhenRequested()
    {
        Assert.Equal(72, Marshal.SizeOf<WindowsDisplayApi.NativePath>());
        Assert.Equal(64, Marshal.SizeOf<WindowsDisplayApi.NativeMode>());
        Assert.Equal(84, Marshal.SizeOf<WindowsDisplayApi.SourceName>());
        Assert.Equal(420, Marshal.SizeOf<WindowsDisplayApi.TargetName>());
        Assert.Equal(276, Marshal.SizeOf<WindowsDisplayApi.AdapterName>());
        Assert.Equal(220, Marshal.SizeOf<WindowsDisplayApi.DevMode>());
        var mode = new SourceMode(Wide, 59, 1, 32, 2);
        var dm = WindowsDisplayApi.DevMode.Request(mode);
        Assert.Equal(mode, dm.Value);
        Assert.Equal(0x7c0080u, dm.Fields);
        Assert.Equal(76, Marshal.OffsetOf<WindowsDisplayApi.DevMode>(nameof(dm.X)).ToInt32());
        Assert.Equal(80, Marshal.OffsetOf<WindowsDisplayApi.DevMode>(nameof(dm.Y)).ToInt32());
        var moved = WindowsDisplayApi.DevMode.Request(mode, new DisplayPosition(-1024, 120));
        Assert.Equal(mode, moved.Value);
        Assert.Equal(dm.Fields | 0x20u, moved.Fields); // DM_POSITION
        Assert.Equal(-1024, moved.X);
        Assert.Equal(120, moved.Y);
    }
}
