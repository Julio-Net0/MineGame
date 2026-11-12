# GLAD OpenGL loader

Generated code; do not edit by hand. Regenerate instead.

- Generator: [glad](https://github.com/Dav1dde/glad) 2.0.8 (`pip install glad2==2.0.8`)
- API: OpenGL core profile 4.3
- Extensions: `GL_ARB_draw_indirect`, `GL_ARB_multi_draw_indirect`
- Language: C, no built-in loader (entry points are loaded through `glfwGetProcAddress`)
- Specification: the copy bundled with the generator (`--reproducible`), so the output does not depend on the network or the date

Command, run from an empty directory:

```
glad --quiet --reproducible --api "gl:core=4.3" --extensions "GL_ARB_multi_draw_indirect,GL_ARB_draw_indirect" --out-path gladout c
```

The output was copied here unchanged apart from line endings (normalised to LF):

| Generated file | Location |
|---|---|
| `include/glad/gl.h` | `src/third_party/glad/include/glad/gl.h` |
| `include/KHR/khrplatform.h` | `src/third_party/glad/include/KHR/khrplatform.h` |
| `src/gl.c` | `src/third_party/glad/src/gl.c` |

Version 4.3 is the newest any render tier calls. The game creates a 3.3 core context; functions the driver does not provide stay null, and the backend checks the context version and extension flags before using anything newer than 3.3.

Licence: the generated loader is `WTFPL OR CC0-1.0`; `khrplatform.h` and the specification-derived declarations are `Apache-2.0` (Khronos).
