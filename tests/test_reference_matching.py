import importlib.util
from pathlib import Path
import unittest
spec=importlib.util.spec_from_file_location('compare',Path(__file__).with_name('compare_onnx.py'))
m=importlib.util.module_from_spec(spec);spec.loader.exec_module(m)
class MatchingTests(unittest.TestCase):
    def test_duplicate_boxes_cannot_all_count_as_matches(self):
        refs=[[50,50,100,100]]
        people=[{'box':[0,0,100,100]} for _ in range(8)]
        self.assertEqual(len(m.match_people(refs,people)),1)
    def test_separate_people_each_match_once(self):
        refs=[[50,50,100,100],[250,50,100,100]]
        people=[{'box':[0,0,100,100]},{'box':[200,0,100,100]}]
        self.assertEqual(len(m.match_people(refs,people)),2)
if __name__=='__main__':unittest.main()
