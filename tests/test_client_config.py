import unittest

# Mirrors pad/main.c cfg_load format: "ip=a.b.c.d" or "ip=any".
def parse_cfg(text):
    for line in text.splitlines():
        line = line.strip()
        if line.startswith('ip='):
            return line[3:].strip()
    return ''

def valid_ip(ip):
    if ip in ('', 'any', '*.*.*.*'):
        return True
    parts = ip.split('.')
    return len(parts) == 4 and all(p.isdigit() and 0 <= int(p) <= 255 for p in parts)

class TestClientConfig(unittest.TestCase):
    def test_parse(self):
        self.assertEqual(parse_cfg('ip=192.168.1.20\n'), '192.168.1.20')
        self.assertEqual(parse_cfg('garbage\nip=any\n'), 'any')
        self.assertEqual(parse_cfg(''), '')
    def test_valid(self):
        for ip in ('', 'any', '*.*.*.*', '192.168.1.1', '0.0.0.0', '255.255.255.255'):
            self.assertTrue(valid_ip(ip), ip)
        for ip in ('1.2.3', '1.2.3.4.5', 'a.b.c.d', '999.1.1.1', '-1.0.0.0'):
            self.assertFalse(valid_ip(ip), ip)

class TestAppCfg(unittest.TestCase):
    def test_cfg_fields(self):
        cfg = open('/home/wozt/dev/wup-thinclient/pad/app.cfg').read()
        for key in ('name', 'net', 'large', 'icon', 'define'):
            self.assertIn(key, cfg)
        self.assertIn('WUP ThinClient', cfg)

if __name__ == '__main__':
    unittest.main()
