"""Metadata-only unit tests; no generated metadata is GUI acceptance evidence."""
import hashlib
import importlib.util
import json
from pathlib import Path
import tempfile
import unittest

spec=importlib.util.spec_from_file_location("pc11_terminal",Path(__file__).with_name("pc11_gui_terminal.py"))
terminal=importlib.util.module_from_spec(spec); spec.loader.exec_module(terminal)


class TerminalGuards(unittest.TestCase):
    def make(self, directory, revision=1, previous="0"*64, path=None):
        path=path or directory/"job.json"
        record=dict(jobId=directory.name,projectId=directory.parent.name,nativeJobRecordRef=str(path),
                    nativeJobRegistrySchema="range.native-job-record/v1",nativeJobRegistryRevision=revision,
                    nativeJobRegistryPreviousSha256=previous,state="running",ownerPid=123)
        compact=lambda value:json.dumps(value,sort_keys=True,separators=(",",":"),ensure_ascii=False).encode()
        record["nativeJobRegistrySha256"]=hashlib.sha256(compact(record)).hexdigest()
        path.write_bytes(compact(record))
        return record

    def test_atomic_snapshot_self_hash(self):
        with tempfile.TemporaryDirectory() as root:
            directory=Path(root)/"project"/"job"; directory.mkdir(parents=True)
            record=self.make(directory,revision=3,previous="a"*64)
            read,proof=terminal.registry(directory)
            self.assertEqual(read,record)
            self.assertEqual(proof[0]["storage"],"atomic-native-job-snapshot")

    def test_atomic_snapshot_tamper_rejected(self):
        with tempfile.TemporaryDirectory() as root:
            directory=Path(root)/"project"/"job"; directory.mkdir(parents=True)
            self.make(directory)
            path=directory/"job.json"; path.write_bytes(path.read_bytes().replace(b'"running"',b'"succeeded"'))
            with self.assertRaisesRegex(ValueError,"self-hash"):
                terminal.registry(directory)

    def test_marker_blob_chain(self):
        with tempfile.TemporaryDirectory() as root:
            directory=Path(root)/"project"/"job"; directory.mkdir(parents=True)
            previous="0"*64
            for revision in (1,2):
                identifier=("a" if revision==1 else "b")*32
                blob=directory/("job-record-r%08d-%s.json"%(revision,identifier))
                record=self.make(directory,revision,previous,blob)
                digest=hashlib.sha256(blob.read_bytes()).hexdigest()
                (directory/("commit-r%08d-%s-%s-%s.marker"%(revision,identifier,previous,digest))).write_bytes(b"")
                previous=record["nativeJobRegistrySha256"]
            read,proof=terminal.registry(directory)
            self.assertEqual(read["nativeJobRegistryRevision"],2)
            self.assertEqual(len(proof),2)

    def test_nonempty_marker_rejected(self):
        with tempfile.TemporaryDirectory() as root:
            directory=Path(root)/"project"/"job"; directory.mkdir(parents=True)
            identifier="a"*32; blob=directory/("job-record-r00000001-"+identifier+".json")
            self.make(directory,path=blob)
            digest=hashlib.sha256(blob.read_bytes()).hexdigest()
            marker=directory/("commit-r00000001-%s-%s-%s.marker"%(identifier,"0"*64,digest))
            marker.write_bytes(b"unexpected")
            with self.assertRaises(ValueError): terminal.registry(directory)

    def test_no_registry_waits_without_provider(self):
        with tempfile.TemporaryDirectory() as root:
            result=terminal.terminal(Path(root),123)
            self.assertEqual(result["status"],"WAITING")


if __name__=="__main__":unittest.main()
