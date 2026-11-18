// No Python twin: reading a texture that may not be CPU-readable (through a temporary RenderTexture), and a texture as
// PNG bytes for the localhost checkout page's logo route.
#nullable enable
using System;
using UnityEngine;

namespace Scfg
{
    internal static class ScfgImages
    {
        /// <summary>The texture as PNG, or null when it cannot be encoded. A texture that is not CPU-readable is read
        /// through a temporary RenderTexture first (<see cref="ReadPixels"/>: that needs a graphics device, null without one).</summary>
        public static byte[]? EncodePng(Texture2D texture)
        {
            try
            {
                if (texture.isReadable) return texture.EncodeToPNG();
                var pixels = ReadPixels(texture, new RectInt(0, 0, texture.width, texture.height));
                if (pixels == null) return null;
                var copy = new Texture2D(texture.width, texture.height, TextureFormat.RGBA32, false);
                try
                {
                    copy.SetPixels32(pixels);
                    copy.Apply();
                    return copy.EncodeToPNG();
                }
                finally
                {
                    ScfgObjects.Destroy(copy);
                }
            }
            catch (Exception e)
            {
                SCFG.ReportError("cannot encode the logo as PNG:", e);
                return null;
            }
        }

        /// <summary>
        /// A rect of a texture's pixels (row by row from the bottom), or null when no graphics device can read it: read
        /// directly from a readable, uncompressed texture, else through a temporary RenderTexture (Blit, ReadPixels) in
        /// the texture's own color space.
        /// </summary>
        public static Color32[]? ReadPixels(Texture2D texture, RectInt rect)
        {
            if (texture.isReadable && !IsCrunched(texture.format))
            {
                try
                {
                    var colors = texture.GetPixels(rect.x, rect.y, rect.width, rect.height);
                    var pixels = new Color32[colors.Length];
                    for (int i = 0; i < colors.Length; i++) pixels[i] = colors[i];
                    return pixels;
                }
                catch (Exception)
                {
                    // a format the CPU cannot read: through the GPU below
                }
            }
            if (SystemInfo.graphicsDeviceType == UnityEngine.Rendering.GraphicsDeviceType.Null) return null;
            var rt = RenderTexture.GetTemporary(texture.width, texture.height, 0, RenderTextureFormat.ARGB32,
                texture.isDataSRGB ? RenderTextureReadWrite.sRGB : RenderTextureReadWrite.Linear);
            var previous = RenderTexture.active;
            var read = new Texture2D(rect.width, rect.height, TextureFormat.RGBA32, false);
            try
            {
                Graphics.Blit(texture, rt);
                RenderTexture.active = rt;
                read.ReadPixels(new Rect(rect.x, rect.y, rect.width, rect.height), 0, 0);
                return read.GetPixels32();
            }
            finally
            {
                RenderTexture.active = previous;
                RenderTexture.ReleaseTemporary(rt);
                ScfgObjects.Destroy(read);
            }
        }

        private static bool IsCrunched(TextureFormat f) =>
            f == TextureFormat.DXT1Crunched || f == TextureFormat.DXT5Crunched || f == TextureFormat.ETC_RGB4Crunched
            || f == TextureFormat.ETC2_RGBA8Crunched;
    }
}
