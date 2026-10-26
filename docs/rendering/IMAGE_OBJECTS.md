# Image objects
Image objects are simple quads that display an image in them. These are usually used in orthogonal scenes to display animated effects.

Image objects with effects ping-pong between two framebuffers, `_rt_imageLayerComposite_{id}_a` and `_b`. Only layers with a named buffer
(a blend mode, `ledsource`, fog, some puppets, or listed in another object's `dependencies`) get a named `_a` other objects can read; the rest share
pooled buffers.

There's special textures used for various things:
- _rt_FullFrameBuffer: the scene as drawn so far
- _rt_MipMappedFrameBuffer: the scene with mipmaps, for reflections
- _rt_4FrameBuffer, _rt_8FrameBuffer, _rt_Bloom: the bloom chain
- _rt_FullAlphaMask: the clipping masks of puppets
