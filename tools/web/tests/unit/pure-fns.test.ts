/* 纯数据函数单测（M5 ESM）：直接 import 源分片——store 的访问器桥在
 * node 下同样生效（store.js 是首个 import），断言与实现精确对齐。
 * UI 行为（I-1 播放头合成层通道、I-2 脏检查 DOM 写）由 tests/e2e 覆盖。 */
import { describe, it, expect, beforeAll } from "vitest";
import { S } from "@web/shared/store.js";
import { escHtml } from "@web/shared/dom.js";
import { fmtTime, activeScene } from "@web/shared/time.js";
import {
  capSegments, sceneTexts, capTrackSub, snapTime, hashSeed, splitSceneData,
} from "@web/timeline/model.js";
import { scriptTokenize } from "@web/shared/tokenize.js";

describe("fmtTime（时间码）", () => {
  it("零与负数", () => {
    expect(fmtTime(0)).toBe("00:00.00");
    expect(fmtTime(-5)).toBe("00:00.00");
    expect(fmtTime(undefined)).toBe("00:00.00");
  });
  it("常规换算", () => {
    expect(fmtTime(65.436)).toBe("01:05.44");
    expect(fmtTime(10.5)).toBe("00:10.50");
  });
  it("百分秒进位（cs=100 → 整秒 +1）", () => {
    expect(fmtTime(125.999)).toBe("02:06.00");
  });
});

describe("escHtml（XSS 转义）", () => {
  it("转义五类字符", () => {
    expect(escHtml('<a href="x">&\'</a>'))
      .toBe("&lt;a href=&quot;x&quot;&gt;&amp;&#39;&lt;/a&gt;");
  });
  it("null/undefined 安全", () => {
    expect(escHtml(null)).toBe("");
    expect(escHtml(undefined)).toBe("");
  });
});

describe("hashSeed（确定性波形种子）", () => {
  it("确定性：同输入同输出", () => {
    expect(hashSeed("abc")).toBe(hashSeed("abc"));
  });
  it("已知值与值域 [0, 1000/37)", () => {
    expect(hashSeed("a")).toBeCloseTo(97 / 37, 12);
    const v = hashSeed("xyz");
    expect(v).toBeGreaterThanOrEqual(0);
    expect(v).toBeLessThan(1000 / 37);
  });
});

describe("activeScene（播放头所在场景）", () => {
  beforeAll(() => {
    S.state.scenes = [
      { id: "s1", start: 0, dur: 2 },
      { id: "s2", start: 2, dur: 3 },
    ];
  });
  it("左闭右开区间", () => {
    expect(activeScene(0).id).toBe("s1");
    expect(activeScene(1.999).id).toBe("s1");
    expect(activeScene(2).id).toBe("s2");
    expect(activeScene(4.999).id).toBe("s2");
  });
  it("区间外为 null", () => {
    expect(activeScene(5)).toBe(null);
    expect(activeScene(-0.1)).toBe(null);
  });
});

describe("capSegments / sceneTexts / capTrackSub（字幕数据源）", () => {
  it("segments 过滤空文本；enabled=false 关闭", () => {
    S.state.spec = {
      captions: { segments: [
        { text: "你好" }, { text: "  " }, null, { text: "世界" },
      ] },
    };
    expect(capSegments().map((s) => s.text)).toEqual(["你好", "世界"]);
    S.state.spec.captions.enabled = false;
    expect(capSegments()).toEqual([]);
    delete S.state.spec.captions.enabled;
  });
  it("sceneTexts 的 subtitle > text > quote > items 回退链", () => {
    S.state.spec = { scenes: [
      { subtitle: "S", text: "T" },
      { text: "T2" },
      { quote: "Q" },
      { items: ["a", "b"] },
      {},
    ] };
    expect(sceneTexts()).toEqual(["S", "T2", "Q", "a · b", ""]);
  });
  it("capTrackSub 的三态文案", () => {
    S.state.spec = { captions: { segments: [{ text: "x" }] }, scenes: [] };
    expect(capTrackSub()).toBe("1 段 · captions.segments");
    S.state.spec.captions = {};
    S.state.spec.scenes = [{ text: "a" }, { text: "" }];
    expect(capTrackSub()).toBe("1 段 · scenes[].text");
    S.state.spec.scenes = [];
    expect(capTrackSub()).toBe("无字幕");
  });
});

describe("splitSceneData（刀口切割，纯数据变换）", () => {
  it("普通场景：前后时长守恒，接缝硬切", () => {
    const sc = { id: "s", duration: 10, transition: "fade", animation: "up" };
    const tail = splitSceneData(sc, 3.456, 10);
    expect(sc.duration).toBe(3.46);          // 刀口按 2 位小数取整
    expect(tail.duration).toBe(6.54);        // 10 - 3.46，总和不变
    expect(tail.transition).toBe("none");    // 接缝必须硬切
    expect(tail.animation).toBe("none");
    expect(tail).not.toBe(sc);               // 深拷贝，独立演化
    expect(tail.id).toBe("s");
  });
  it("视频场景：video_from 累加刀口，对象形式归一", () => {
    const sc = { id: "v", duration: 8, video: "clip.mp4", video_from: 1.5 };
    const tail = splitSceneData(sc, 2, 8);
    expect(tail.video_from).toBe(3.5);       // 1.5 + 2，从源视频该时刻继续
    // 场景级 video_from 缺失时才回落到对象形式 video.from
    const sc2 = { id: "v2", duration: 8, video: { from: 4 } };
    const tail2 = splitSceneData(sc2, 1, 8);
    expect(tail2.video_from).toBe(5);
    expect(tail2.video.from).toBeUndefined();
  });
});

describe("scriptTokenize（逐词面板词元）", () => {
  it("CJK 逐字、其余按词、空格不产出词元", () => {
    expect(scriptTokenize("你好 world")).toEqual(["你", "好", "world"]);
    expect(scriptTokenize("abc def")).toEqual(["abc", "def"]);
  });
  it("空文本保底返回原文本", () => {
    expect(scriptTokenize("")).toEqual([""]);
  });
});

describe("snapTime（时间轴吸附）", () => {
  beforeAll(() => {
    S.state.spec = { composition: { fps: 30 } };
    S.state.scenes = [{ id: "s", start: 2, dur: 3 }];
    S.state.duration = 10;
  });
  it("吸附到场景边界（阈值 0.085s 内）", () => {
    expect(snapTime(2.03)).toBe(2);
    expect(snapTime(4.97)).toBe(5);
  });
  it("不吸附时按帧取整并夹在 [0, duration]", () => {
    expect(snapTime(2.5)).toBe(2.5);
    expect(snapTime(-1)).toBe(0);
    expect(snapTime(99)).toBe(10);
  });
});
