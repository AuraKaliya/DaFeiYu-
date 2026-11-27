"""对照未送模的字符二元组候选。传入问句 JSONL、--measure-paraphrases 的输出及该轮档案根目录。"""
import argparse
import json
import sqlite3
from collections import defaultdict
from pathlib import Path


# 保留编号和标点，只统一全角 ASCII 及英文大小写。
def fold(text):
    narrow = "".join(chr(ord(c) - 0xfee0) if "！" <= c <= "～" else c for c in text.replace("　", " "))
    return "".join(c.lower() if "A" <= c <= "Z" else c for c in narrow.strip(" "))


# 生成相邻字符集合，单字符无法提供二元组依据。
def grams(text):
    return set(zip(text, text[1:]))


# 只为有在读原件的比较建立试验索引，保留条件和对象的不同用途。
def read_index(path):
    anchors, qualifiers = defaultdict(list), defaultdict(set)
    with sqlite3.connect(path.resolve().as_uri() + "?mode=ro", uri=True) as db:
        for name, kind, term in db.execute(
            "SELECT name,kind,term FROM case_term t WHERE EXISTS "
            "(SELECT 1 FROM case_member m WHERE m.name=t.name AND m.active=1)"
        ):
            if kind in {"name", "object", "alias"}:
                anchors[name].append(grams(fold(term)))
            else:
                qualifiers[name].add(fold(term))
        for name, checkpoint in db.execute("SELECT DISTINCT name,checkpoint FROM case_member WHERE active=1"):
            qualifiers[name].add(fold(checkpoint))
    return anchors, qualifiers


# 在同组完整条件约束下按 Dice 系数排序，最高分并列时只请求澄清。
def rank(topics, anchors, qualifiers):
    words = [fold(t) for t in topics]
    if any("/" in t for t in words) or not (query := grams(words[0])):
        return 0, "-"
    best, choice = 0, "-"
    for name, labels in anchors.items():
        if not all(t in qualifiers[name] for t in words[1:]):
            continue
        score = max(2 * len(query & label) / (len(query) + len(label)) for label in labels)
        if score > best:
            best, choice = score, name
        elif score == best:
            choice = "?"
    return best, choice


# 只在正式入口实测无结果的位置尝试补查，金标仅用于最后计分。
def compare(corpus, log, archives):
    rows = [json.loads(line) for line in corpus.read_text(encoding="utf-8").splitlines()]
    decisions = {}
    for line in log.read_text(encoding="utf-8-sig").splitlines():
        if line.startswith("observed "):
            _, scale, key, result = line.split()
            decisions[int(scale), key] = result
    for scale in (32, 512, 4096):
        anchors, qualifiers = read_index(archives / str(scale) / "main-conversation.sqlite3")
        results = {cut: [] for cut in (0.15, 0.30, 0.50, 0.70)}
        for row in rows:
            actual = decisions[scale, row["id"]]
            score, candidate = rank(row["topics"], anchors, qualifiers) if actual == "-" else (0, "-")
            for cut, scores in results.items():
                selected = candidate if actual == "-" and score >= cut else actual
                scores.append((row, selected))
        for cut, scores in results.items():
            hits = sum(row.get("target") == choice for row, choice in scores)
            wrong = [row["id"] + "=" + choice for row, choice in scores if choice not in {"-", "?"} and row.get("target") != choice]
            total = sum("target" in row for row in rows)
            print(f"{scale} Dice {cut:.2f} hits {hits}/{total}, wrong {len(wrong)}", ",".join(wrong))


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    for arg in ("corpus", "log", "archives"):
        parser.add_argument(arg, type=Path)
    args = parser.parse_args()
    compare(args.corpus, args.log, args.archives)
