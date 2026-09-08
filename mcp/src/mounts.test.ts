import assert from "node:assert/strict";
import test from "node:test";

import { parseMounts } from "./mounts.js";

const CONF = `
[sdl]
autolock=true

[autoexec]
# a comment
mount C "/host/dos c"
MOUNT -t dir D /host/build
c:
QRENDER.EXE dm3ish.bsp
`;

test("parseMounts reads MOUNT lines out of [autoexec] only", () => {
  assert.deepEqual(parseMounts(CONF), [
    { drive: "C", hostPath: "/host/dos c" },
    { drive: "D", hostPath: "/host/build" },
  ]);
});
