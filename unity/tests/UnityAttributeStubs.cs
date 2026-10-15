// Offline-test-only stand-ins for the three inspector attributes
// ProcessingSettings.cs uses, so the Unity-free DSP core compiles outside
// Unity. Never add this file to the Unity project.
using System;

namespace UnityEngine
{
    [AttributeUsage(AttributeTargets.Field)]
    public sealed class RangeAttribute : Attribute { public RangeAttribute(float min, float max) { } }

    [AttributeUsage(AttributeTargets.Field)]
    public sealed class HeaderAttribute : Attribute { public HeaderAttribute(string header) { } }

    [AttributeUsage(AttributeTargets.Field)]
    public sealed class TooltipAttribute : Attribute { public TooltipAttribute(string tooltip) { } }
}
