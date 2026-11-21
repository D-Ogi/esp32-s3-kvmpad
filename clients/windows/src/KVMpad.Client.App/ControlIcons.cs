// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// Copyright 2026 D-Ogi. Noncommercial use only; see LICENSE.md.
// Required Notice: Copyright 2026 D-Ogi (https://github.com/D-Ogi/esp32-s3-kvmpad)
using System;
using System.Collections.Generic;
using System.Drawing;
using System.Drawing.Drawing2D;
using System.Drawing.Imaging;
using System.IO;
using System.Runtime.InteropServices;
using System.Windows.Forms;

namespace KVMpad.Client.App;

internal enum IconRole { Neutral, Client, Host }

internal sealed class ControlIcons : IDisposable
{
    private readonly List<(Button Button, string Name, IconRole Role)> _buttons = [];
    private readonly string _directory;
    private (int Size, bool Dark, bool Contrast, int Ink)? _last;

    public ControlIcons(string directory) => _directory = directory;

    public void Add(Button button, string name, IconRole role)
    {
        button.TextImageRelation = TextImageRelation.ImageBeforeText;
        button.ImageAlign = ContentAlignment.MiddleLeft;
        _buttons.Add((button, name, role));
        _last = null;
    }

    internal static Color Ink(IconRole role, bool dark, bool contrast, Color systemInk)
        => contrast ? systemInk : (role, dark) switch {
            (IconRole.Client, false) => Color.FromArgb(0, 106, 203),
            (IconRole.Client, true) => Color.FromArgb(132, 191, 255),
            (IconRole.Host, false) => Color.FromArgb(33, 114, 77),
            (IconRole.Host, true) => Color.FromArgb(119, 214, 174),
            (_, false) => Color.FromArgb(29, 29, 31),
            (_, true) => Color.FromArgb(229, 231, 235),
        };

    internal static Bitmap Render(string path, int size, Color ink)
    {
        using var source = Image.FromFile(path);
        using var attributes = new ImageAttributes();
        attributes.SetColorMatrix(new ColorMatrix {
            Matrix00 = 0, Matrix11 = 0, Matrix22 = 0,
            Matrix40 = ink.R / 255f, Matrix41 = ink.G / 255f, Matrix42 = ink.B / 255f
        });
        attributes.SetWrapMode(WrapMode.TileFlipXY);
        var bitmap = new Bitmap(size, size, PixelFormat.Format32bppPArgb);
        using var g = Graphics.FromImage(bitmap);
        g.CompositingMode = CompositingMode.SourceCopy;
        g.InterpolationMode = InterpolationMode.HighQualityBilinear;
        g.PixelOffsetMode = PixelOffsetMode.Half;
        g.DrawImage(source, new Rectangle(0, 0, size, size), 0, 0, source.Width, source.Height,
                    GraphicsUnit.Pixel, attributes);
        return bitmap;
    }

    public void Apply(int dpi, bool dark, bool contrast, Color systemInk)
    {
        int size = Math.Max(1, (20 * dpi + 48) / 96);
        var key = (size, dark, contrast, systemInk.ToArgb());
        if (_last == key) return;
        _last = key;
        foreach (var (button, name, role) in _buttons) {
            Image? next = null;
            try {
                string path = Path.Combine(_directory, name + ".png");
                if (File.Exists(path)) next = Render(path, size, Ink(role, dark, contrast, systemInk));
            }
            catch (Exception ex) when (ex is IOException or UnauthorizedAccessException or ArgumentException or ExternalException) {
                // An incomplete installation can still use the same labeled action.
            }
            Image? previous = button.Image;
            button.Image = next;
            previous?.Dispose();
        }
    }

    public void Dispose()
    {
        foreach (var (button, _, _) in _buttons) {
            var image = button.Image;
            button.Image = null;
            image?.Dispose();
        }
        _buttons.Clear();
    }
}
