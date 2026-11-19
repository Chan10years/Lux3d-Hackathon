import importlib.util
import json
from pathlib import Path
import unittest

spec = importlib.util.spec_from_file_location('lan_discovery', Path(__file__).parents[1] / 'scripts/lan_discovery.py')
lan = importlib.util.module_from_spec(spec)
spec.loader.exec_module(lan)


class DiscoveryTests(unittest.TestCase):
    def test_request_only_matches_published_code(self):
        request = lan.request_packet('123456', 'a' * 32)
        self.assertIsNone(lan.answer(request, '654321'))
        self.assertIsNone(lan.answer(request, ''))
        self.assertIsNotNone(lan.answer(request, '123456'))

    def test_nonce_version_and_code_are_checked(self):
        request = lan.request_packet('123456', 'a' * 32)
        response = lan.answer(request, '123456')
        self.assertTrue(lan.valid_response(response, '123456', 'a' * 32))
        self.assertFalse(lan.valid_response(response, '123456', 'b' * 32))
        self.assertFalse(lan.valid_response(response, '654321', 'a' * 32))
        changed = json.loads(response)
        changed['version'] = 999
        self.assertFalse(lan.valid_response(json.dumps(changed).encode(), '123456', 'a' * 32))
        self.assertIsNone(lan.answer(json.dumps(changed).encode(), '123456'))

    def test_malformed_and_oversized_packets(self):
        for data in (b'null', b'[]', b'not json', b'x' * 2048):
            self.assertIsNone(lan.answer(data, '123456'))
            self.assertFalse(lan.valid_response(data, '123456', 'a' * 32))

    def test_multiple_interfaces_same_host_deduplicate_by_instance(self):
        responses = [('host-a', '192.168.1.3'), ('host-a', '10.0.0.3')]
        self.assertEqual(len(lan.unique_hosts(responses)), 1)
        responses.append(('host-b', '192.168.1.4'))
        self.assertEqual(len(lan.unique_hosts(responses)), 2)

    def test_timeout_and_conflict_are_explicit(self):
        with self.assertRaisesRegex(RuntimeError, '未找到'):
            lan.choose_host([])
        with self.assertRaisesRegex(RuntimeError, '冲突'):
            lan.choose_host([('a', '192.168.1.2'), ('b', '192.168.1.3')])


if __name__ == '__main__':
    unittest.main()
