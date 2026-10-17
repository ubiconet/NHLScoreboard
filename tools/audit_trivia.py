import re

totals = {}
files = [
    (1, 'src/sports/nhl/trivia_questions_easy.h'),
    (2, 'src/sports/nhl/trivia_questions_medium.h'),
    (3, 'src/sports/nhl/trivia_questions_hard.h'),
]
pat = re.compile(
    r'\{\s*(\d)\s*,\s*(\d)\s*,\s*"([^"]*)"\s*,'
    r'\s*\{\s*"([^"]*)"\s*,\s*"([^"]*)"\s*,\s*"([^"]*)"\s*,\s*"([^"]*)"\s*\}\s*\}'
)
for diff, path in files:
    with open(path, encoding='utf-8') as f:
        txt = f.read()
    entries = pat.findall(txt)
    totals[diff] = len(entries)
    entry_lines = len(re.findall(r'^\s*\{\d,', txt, re.M))
    if entry_lines != len(entries):
        print('%s: PARSE MISMATCH lines=%d parsed=%d' % (path, entry_lines, len(entries)))
    for d, c, q, a1, a2, a3, a4 in entries:
        problems = []
        if int(d) != diff:
            problems.append('wrong-difficulty')
        if int(c) > 3:
            problems.append('correctIndex>3')
        for a in (a1, a2, a3, a4):
            if len(a) > 22:
                problems.append('answer>22: "%s" (%d)' % (a, len(a)))
        if len(q) > 90:
            problems.append('question>90: "%s" (%d)' % (q, len(q)))
        if q.strip() == '' or any(a.strip() == '' for a in (a1, a2, a3, a4)):
            problems.append('empty-field')
        if len(set([a1, a2, a3, a4])) != 4:
            problems.append('dupe-answers: %s' % q)
        if problems:
            print('%s: %s -> %s' % (path.split('/')[-1], q[:50], problems))
print('totals:', totals, 'grand total:', sum(totals.values()))
