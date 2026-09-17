.. This file is a placeholder. ci/report/publish-perf.py overwrites it
.. from the published history during the docs-deploy build; what is
.. committed here is what a local or pull request build renders, since
.. neither has the history.

Performance trends
==================

This page is generated from the recorded history of the nightly
performance runs. The record itself is not kept on ``main`` -- it lives
at ``perf/history.jsonl`` on the ``gh-pages`` branch, written by the
lanes in ``.github/workflows/system-tests.yml`` and read back when the
site is built.

So this copy is empty by construction. The published page, with the
charts and the per-size tables, is at
https://rocm.github.io/rocm-ernic/perf-trends.html.

To render it locally, fetch the history first::

   git show origin/gh-pages:perf/history.jsonl \
       > docs/perf-history/history.jsonl
   python3 ci/report/publish-perf.py --render-only --docs-dir docs
   cmake --build build --target sphinx-html
