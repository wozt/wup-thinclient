import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]

# Mirrors pad/main.c's backward-compatible first-token config parser.
def parse_cfg(text):
    tokens = text.split()
    return tokens[0] if tokens else ''

def valid_ip(ip):
    if ip in ('', 'any', '*.*.*.*'):
        return True
    parts = ip.split('.')
    return len(parts) == 4 and all(p.isdigit() and 0 <= int(p) <= 255 for p in parts)

class TestClientConfig(unittest.TestCase):
    def test_parse(self):
        self.assertEqual(parse_cfg('192.168.1.20 auto=0\n'), '192.168.1.20')
        self.assertEqual(parse_cfg('any auto=1\n'), 'any')
        self.assertEqual(parse_cfg(''), '')
    def test_valid(self):
        for ip in ('', 'any', '*.*.*.*', '192.168.1.1', '0.0.0.0', '255.255.255.255'):
            self.assertTrue(valid_ip(ip), ip)
        for ip in ('1.2.3', '1.2.3.4.5', 'a.b.c.d', '999.1.1.1', '-1.0.0.0'):
            self.assertFalse(valid_ip(ip), ip)

class TestAppCfg(unittest.TestCase):
    def test_cfg_fields(self):
        cfg = (ROOT / 'pad' / 'app.cfg').read_text(encoding='utf-8')
        for key in ('name', 'net', 'large', 'icon', 'define'):
            self.assertIn(key, cfg)
        self.assertIn('WUP ThinClient', cfg)

if __name__ == '__main__':
    unittest.main()
