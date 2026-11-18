// The package's link.xml reaches the linker (Unity ignores a link.xml inside a package).
#nullable enable
using System.IO;
using NUnit.Framework;
using Scfg.Editor;

namespace Scfg.Tests
{
    public sealed class LinkXmlTests
    {
        [Test]
        public void TheLinkerProcessorFindsThePackagesLinkXml()
        {
            var path = ScfgLinkXml.PackageLinkXml();
            Assert.That(path, Is.Not.Null);
            var text = File.ReadAllText(path!);
            Assert.That(text, Does.Contain("<assembly fullname=\"Scfg.Core\" preserve=\"all\" />"));
            Assert.That(new ScfgLinkXml().GenerateAdditionalLinkXmlFile(null!, null!), Is.EqualTo(path));
        }
    }
}
