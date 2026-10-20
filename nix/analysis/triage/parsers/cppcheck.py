"""Parse cppcheck XML reports."""

import xml.parsers.expat
from xml.etree.ElementTree import TreeBuilder

from finding import Finding, normalize_path, normalize_severity


class XMLDoctypeError(ValueError):
    """An XML file has a DOCTYPE, which parse_xml() refuses."""


def parse_xml(path: str):
    """Parse an XML file and return its root element, refusing any DOCTYPE.

    xml.etree.ElementTree.parse() expands the entities a document
    declares in its DOCTYPE, so a small file can expand to gigabytes
    (the "billion laughs" attack). A cppcheck report never has a
    DOCTYPE, so refuse a file that does, before any declaration in it is
    read, the way defusedxml does without the dependency.
    """

    def refuse_doctype(*_args):
        raise XMLDoctypeError(f'{path}: has a DOCTYPE')

    builder = TreeBuilder()
    parser = xml.parsers.expat.ParserCreate()
    parser.StartDoctypeDeclHandler = refuse_doctype
    parser.StartElementHandler = builder.start
    parser.EndElementHandler = builder.end
    parser.CharacterDataHandler = builder.data
    with open(path, 'rb') as fh:
        parser.ParseFile(fh)
    return builder.close()


def parse(path: str) -> list:
    """Parse cppcheck XML report."""
    findings = []
    try:
        root = parse_xml(path)
    except (xml.parsers.expat.ExpatError, XMLDoctypeError,
            FileNotFoundError):
        return findings

    for error in root.iter('error'):
        check_id = error.get('id', '')
        severity = error.get('severity', 'warning')
        msg = error.get('msg', '')

        for loc in error.iter('location'):
            filepath = normalize_path(loc.get('file', ''))
            line = int(loc.get('line', 0))
            if filepath and line > 0:
                findings.append(Finding(
                    tool='cppcheck',
                    check_id=check_id,
                    severity=normalize_severity(severity),
                    file=filepath,
                    line=line,
                    message=msg,
                ))
                break  # Only take first location per error

    return findings
