// No Python twin: a texture as PNG bytes for the localhost checkout page's logo route.
#nullable enable
using System;
using UnityEngine;

namespace Scfg
{
    internal static class ScfgImages
    {
        /// <summary>The texture as PNG, or null when it cannot be encoded. A texture that is not CPU-readable is copied
        /// through a temporary RenderTexture first (that needs a graphics device: null without one).</summary>
        public static byte[]? EncodePng(Texture2D texture)
        {
            try
            {
                if (texture.isReadable) return texture.EncodeToPNG();
                if (SystemInfo.graphicsDeviceType == UnityEngine.Rendering.GraphicsDeviceType.Null) return null;
                var rt = RenderTexture.GetTemporary(texture.width, texture.height, 0, RenderTextureFormat.ARGB32);
                var previous = RenderTexture.active;
                try
                {
                    Graphics.Blit(texture, rt);
                    RenderTexture.active = rt;
                    var copy = new Texture2D(texture.width, texture.height, TextureFormat.RGBA32, false);
                    copy.ReadPixels(new Rect(0, 0, texture.width, texture.height), 0, 0);
                    copy.Apply();
                    var png = copy.EncodeToPNG();
                    UnityEngine.Object.Destroy(copy);
                    return png;
                }
                finally
                {
                    RenderTexture.active = previous;
                    RenderTexture.ReleaseTemporary(rt);
                }
            }
            catch (Exception e)
            {
                SCFG.ReportError("cannot encode the logo as PNG:", e);
                return null;
            }
        }
    }
}
