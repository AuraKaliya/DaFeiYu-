-- 全部事件、库和身份均为合成样本。event 是宿主签发的原事件号，跨库转述沿用同号。
CREATE TABLE grants(agent TEXT, task TEXT, audience TEXT, store TEXT, layer TEXT,
    PRIMARY KEY(agent, task, audience, store, layer)) WITHOUT ROWID;
CREATE TABLE entries(store TEXT, id TEXT, event TEXT, revision INTEGER, layer TEXT,
    topic TEXT, body TEXT, since INTEGER, until INTEGER, copied INTEGER,
    PRIMARY KEY(store, id)) WITHOUT ROWID;
CREATE INDEX recall_scope ON entries(store, layer, topic);
INSERT INTO grants VALUES
    ('main','review','owner','personal','preference'), ('main','review','owner','personal','quote'),
    ('main','review','owner','received','preference'), ('main','review','owner','archive','preference'),
    ('main','review','owner','wiki','knowledge'), ('avatar','reply','public','shared','preference'),
    ('business-a','service','internal','business-a','preference'),
    ('one-shot','job-7','internal','brief-7','knowledge'),
    ('main','travel','owner','itinerary','quote');
-- 每组按序入库，使同一查询在各阶段的可见材料可以逐项比较。
CREATE TABLE batches(batch INTEGER, store TEXT, id TEXT, event TEXT, revision INTEGER,
    layer TEXT, topic TEXT, body TEXT, since INTEGER, until INTEGER, copied INTEGER);
INSERT INTO batches VALUES
    (0,'personal','now','E2',2,'preference','周报','先写结论。',20,NULL,0),
    (0,'personal','words','Q1',1,'quote','周报','周报开头请先告诉我结论。',20,NULL,0),
    (1,'archive','then','E1',1,'preference','周报','先列完整过程。',10,20,0),
    (2,'received','copy-a','E2',2,'preference','周报','一封信转述了先写结论的要求。',21,NULL,1),
    (2,'received','copy-b','E2',2,'preference','周报','同一封信再次转述。',22,NULL,1),
    (3,'business-a','secret','B1',1,'preference','周报','内部业务甲。',10,NULL,0),
    (3,'business-b','secret','B2',1,'preference','周报','内部业务乙。',10,NULL,0),
    (3,'shared','public','P1',1,'preference','周报','公开渠道可引用简版。',10,NULL,0),
    (3,'brief-7','rule','T1',1,'knowledge','交付','只交付核对表。',10,NULL,0),
    (4,'received','independent','E3',1,'preference','周报','协作者希望先写进展。',25,NULL,0),
    (4,'wiki','old','K1',1,'knowledge','归档','保存七天。',10,30,0),
    (4,'wiki','new','K2',2,'knowledge','归档','保存三十天。',30,NULL,0),
    (4,'itinerary','booking','F1',1,'quote','往返行程','预订编号 DFY-42。',11,NULL,0),
    (4,'itinerary','out','F2',1,'quote','往返行程','去程座位 3A。',12,NULL,0),
    (4,'itinerary','back','F3',1,'quote','往返行程','回程座位 7C。',13,NULL,0);
