# pyre-unsafe

import glob
import io
import logging
import os
import zipfile
from typing import Any, Union

import torch
from torch.export.pt2_archive.constants import (
    ARCHIVE_FORMAT_PATH,
    ARCHIVE_FORMAT_VALUE,
    ARCHIVE_VERSION_PATH,
    ARCHIVE_VERSION_VALUE,
)
from torch.types import FileLike


logger: logging.Logger = logging.getLogger(__name__)


def is_pt2_package(serialized_model: Union[bytes, str]) -> bool:
    """
    Check if the serialized model is a PT2 Archive package.
    """
    try:
        zip_reader = zipfile.ZipFile(
            io.BytesIO(serialized_model)
            if isinstance(serialized_model, bytes)
            else serialized_model
        )
        root_folder = zip_reader.namelist()[0].split(os.path.sep)[0]
        archive_format_path = f"{root_folder}/{ARCHIVE_FORMAT_PATH}"
        if archive_format_path in zip_reader.namelist():
            return zip_reader.read(archive_format_path) == b"pt2"
    except Exception as ex:
        logger.info("Model is not a PT2 package: %s", str(ex))
    return False


class PT2ArchiveWriter:
    """
    Context manager for writing a PT2 archive.
    """

    def __init__(self, archive_path_or_buffer: FileLike):
        self.archive_file = torch._C.PyTorchFileWriter(archive_path_or_buffer)  # type: ignore[arg-type]
        # NOTICE: version here is different from the archive_version
        # this is the version of zip file format, which is used by PyTorchFileWriter, which write to /.data/version
        # archive_version is the version of the PT2 archive spec, which write to /archive_version
        self.archive_file.set_min_version(6)

    def __enter__(self) -> "PT2ArchiveWriter":
        return self

    def __exit__(self, *args: Any) -> None:
        if not self.has_record(ARCHIVE_FORMAT_PATH):
            self.write_string(ARCHIVE_FORMAT_PATH, ARCHIVE_FORMAT_VALUE)

        if not self.has_record(ARCHIVE_VERSION_PATH):
            self.write_string(ARCHIVE_VERSION_PATH, ARCHIVE_VERSION_VALUE)

        self.close()

    def has_record(self, name: str) -> bool:
        """
        Check if a record exists in the archive.
        """
        return name in self.archive_file.get_all_written_records()

    def count_prefix(self, prefix: str) -> int:
        """
        Count the number of records that start with a given prefix.
        """
        return sum(
            1
            for record in self.archive_file.get_all_written_records()
            if record.startswith(prefix)
        )

    def write_bytes(self, name: str, data: bytes) -> None:
        """
        Write a bytes object to the archive.
        name: The destination file inside the archive.
        data: The bytes object to write.
        """
        assert isinstance(data, bytes), f"Expected bytes but got {type(data)}"
        self.archive_file.write_record(name, data, len(data))

    def write_string(self, name: str, data: str) -> None:
        """
        Write a string object to the archive.
        name: The destination file inside the archive.
        data: The string object to write.
        """
        assert isinstance(data, str), f"Expected string but got {type(data)}"
        data_bytes = data.encode()
        self.write_bytes(name, data_bytes)

    def write_file(self, name: str, file_path: str) -> None:
        """
        Copy a file into the archive.
        name: The destination file inside the archive.
        file_path: The source file on disk.
        """
        assert os.path.isfile(file_path), f"{file_path} is not a valid file path"

        with open(file_path, "rb") as f:
            file_bytes = f.read()
            self.write_bytes(name, file_bytes)

    def write_folder(self, archive_dir: str, folder_dir: str) -> None:
        """
        Copy a folder into the archive.
        archive_dir: The destination folder inside the archive.
        folder_dir: The source folder on disk.
        """
        assert os.path.isdir(folder_dir), f"{folder_dir} is not a valid directory path"

        file_paths = filter(
            os.path.isfile, glob.glob(f"{folder_dir}/**", recursive=True)
        )
        for file_path in file_paths:
            filename = os.path.relpath(file_path, folder_dir)
            archive_path = os.path.join(archive_dir, filename)
            self.write_file(archive_path, file_path)

    def close(self) -> None:
        """
        Close the archive.
        """
        self.archive_file.write_end_of_file()


class PT2ArchiveReader:
    """
    Context manager for reading a PT2 archive.
    """

    def __init__(self, archive_path_or_buffer: FileLike):
        self.archive_file = torch._C.PyTorchFileReader(archive_path_or_buffer)  # type: ignore[arg-type]
        assert (
            self.read_string(ARCHIVE_FORMAT_PATH) == ARCHIVE_FORMAT_VALUE
        ), "Invalid archive format"

    def __enter__(self) -> "PT2ArchiveReader":
        return self

    def __exit__(self, *args: Any) -> None:
        # torch._C.PyTorchFileReader doesn't have a close method
        pass

    def read_bytes(self, name: str) -> bytes:
        """
        Read a bytes object from the archive.
        name: The source file inside the archive.
        """
        return self.archive_file.get_record(name)

    def read_string(self, name: str) -> str:
        """
        Read a string object from the archive.
        name: The source file inside the archive.
        """
        data = self.read_bytes(name)
        return data.decode()

    def archive_version(self) -> int:
        """
        Get the archive version.
        """
        try:
            archive_version = self.read_string(ARCHIVE_VERSION_PATH)
        except Exception:
            # if archive_version is not found, it means the archive is older than version 0.
            # In this case, we assume the archive is version 0.
            archive_version = "0"

        return int(archive_version)
