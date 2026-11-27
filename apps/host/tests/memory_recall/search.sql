-- 宿主固定调用者、当前委托、受众和读取层；查询词只能在这些授权库内匹配。
WITH eligible AS (
    SELECT e.*, (e.topic = :term) AS exact_topic
    FROM grants AS g
    CROSS JOIN entries AS e ON e.store = g.store AND e.layer = g.layer
    WHERE g.agent = :agent AND g.task = :task AND g.audience = :audience
      AND g.layer = :layer AND :term <> ''
      AND e.since <= :as_of AND (e.until IS NULL OR :as_of < e.until)
      AND (e.topic = :term OR instr(e.body, :term) > 0)
), copies AS (
    SELECT *, row_number() OVER (
        PARTITION BY store, event ORDER BY exact_topic DESC, copied, revision DESC, id
    ) AS occurrence FROM eligible
), ranked AS (
    SELECT *, row_number() OVER (
        PARTITION BY store ORDER BY exact_topic DESC, copied, since DESC, id
    ) AS local_rank FROM copies WHERE occurrence = 1
)
SELECT store, id, event, revision, body
FROM ranked WHERE local_rank <= :per_store
ORDER BY exact_topic DESC, local_rank, copied, since DESC, store, id;
