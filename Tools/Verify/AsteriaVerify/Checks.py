# SPDX-License-Identifier: Apache-2.0
"""Registry of target verification checks.

Two kinds of checks exist.

A *function check* is a function `f(context) -> list[str]` registered with
`@check`. It returns problem descriptions; an empty list is a pass.

A *property* is a subclass of `Property` registered with `@register`. It
separates extracting facts from an artifact (`extract`) from judging them
(`verify`), and names deliberately broken variants of the extracted facts
(`mutants`). The runner applies `verify` to the real facts, which must pass,
and to every mutant, which must fail, so each property proves on every run
that it can detect the defects it exists for. A mutant equal to the real
facts is itself an error: it would prove nothing. A property may also
summarize passing facts (`summary`), such as totals it proved; the runner
prints the summaries with --verbose.

Modules under AsteriaVerify/TargetChecks register their checks on import;
`registry` imports them all.
"""
from __future__ import annotations

import dataclasses
import importlib
import pathlib
import pkgutil
from typing import Any, Callable, Iterable

_REGISTRY: list['Check'] = []


@dataclasses.dataclass
class Outcome:
    problems: list[str]
    mutants_rejected: int = 0
    skipped: str | None = None
    notes: list[str] = dataclasses.field(default_factory=list)


class Property:
    """Base class of mutation-tested properties.

    Subclasses optionally set `name` (by default the class name), `isas` and
    `variants`, and implement `subjects`, `extract`, `verify` and `mutants`.
    """
    name = ''
    isas: tuple[str,...] = ('Ia64', 'Amd64')
    # Variants the property needs; the property is skipped in trees that
    # lack all of them and runs once per subject otherwise.
    variants: tuple[str,...] = ('firmware', 'O0', 'O2')

    def subjects(self, context) -> Iterable[Any]:
        """The artifacts to check; by default each available variant."""
        return [variant for variant in self.variants if context.has_variant(variant)]

    def describe(self, subject: Any) -> str:
        return str(subject)

    def extract(self, context, subject: Any) -> Any:
        raise NotImplementedError

    def verify(self, facts: Any) -> list[str]:
        raise NotImplementedError

    def mutants(self, facts: Any) -> Iterable[tuple[str, Any]]:
        return ()

    def summary(self, facts: Any) -> str | None:
        """A note on passing facts, printed with --verbose."""
        return None


class AcceptanceError(Exception):
    """A legal variant of the facts, which a property must accept, was
    rejected: the property is too strict."""


class ItemProperty(Property):
    """A property whose facts map item names to the facts of each item;
    problems and mutants are judged per item and prefixed with its name."""

    def item_problems(self, item: Any) -> list[str]:
        raise NotImplementedError

    def item_mutants(self, item: Any) -> Iterable[tuple[str, Any]]:
        return ()

    def verify(self, facts: dict) -> list[str]:
        return ['%s: %s' % (name, problem) for name, item in facts.items() for problem in self.item_problems(item)]

    def mutants(self, facts: dict) -> Iterable[tuple[str, Any]]:
        for name, item in facts.items():
            for description, changed in self.item_mutants(item):
                yield '%s: %s' % (name, description), {**facts, name: changed}


@dataclasses.dataclass
class Check:
    name: str
    isas: tuple[str,...]
    function: Callable | None = None
    uses_golden: bool = False
    prop: Property | None = None
    variants: tuple[str,...] = ()

    def applies_to(self, context) -> bool:
        return context.isa in self.isas

    def run(self, context, *, update_golden: bool = False, goldens: list[str] | None = None) -> Outcome:
        needed = self.prop.variants if self.prop is not None else self.variants
        if needed and not any(context.has_variant(variant) for variant in needed):
            return Outcome([], skipped='needs one of the variants %s' % ', '.join(needed))
        if self.prop is None:
            if self.uses_golden:
                return Outcome(list(self.function(context, update_golden=update_golden, goldens=goldens or [])))
            return Outcome(list(self.function(context)))
        return _run_property(self.prop, context)


def _run_property(prop: Property, context) -> Outcome:
    problems: list[str] = []
    notes: list[str] = []
    rejected = 0
    for subject in prop.subjects(context):
        label = prop.describe(subject)
        facts = prop.extract(context, subject)
        found = prop.verify(facts)
        if found:
            problems.extend('%s: %s' % (label, problem) for problem in found)
            continue
        note = prop.summary(facts)
        if note:
            notes.append('%s: %s' % (label, note))
        names = set()
        for description, mutant in prop.mutants(facts):
            if description in names:
                problems.append('%s: self-test declares mutant "%s" twice' % (label, description))
            names.add(description)
            if mutant == facts:
                problems.append('%s: self-test mutant "%s" does not change the facts' % (label, description))
            elif not prop.verify(mutant):
                problems.append('%s: self-test mutant "%s" was accepted' % (label, description))
            else:
                rejected += 1
    return Outcome(problems, rejected, notes=notes)


def check(name: str, *, isas: tuple[str,...] = ('Ia64', 'Amd64'), uses_golden: bool = False,
          variants: tuple[str,...] = ()):
    """Registers a function check."""
    def register_function(function: Callable) -> Callable:
        _add(Check(name, isas, function, uses_golden, variants=variants))
        return function
    return register_function


def register(cls: type[Property]) -> type[Property]:
    """Registers a Property subclass (instantiated once); the check is named
    after the class unless the class sets `name`."""
    cls.name = cls.__dict__.get('name') or cls.__name__
    _add(Check(cls.name, tuple(cls.isas), prop=cls()))
    return cls


def _add(entry: Check) -> None:
    if any(existing.name == entry.name for existing in _REGISTRY):
        raise ValueError('duplicate check name %s' % entry.name)
    _REGISTRY.append(entry)


def registry() -> list[Check]:
    """Imports every check module. A module that fails to import becomes a
    failing check named `Import:<module>` instead of aborting the run."""
    package = pathlib.Path(__file__).resolve().parent / 'TargetChecks'
    for module in sorted(pkgutil.iter_modules([str(package)]), key=lambda m: m.name):
        try:
            importlib.import_module('AsteriaVerify.TargetChecks.' + module.name)
        except Exception as error:  # Reported as a check failure
            message = '%s: %s' % (type(error).__name__, error)
            if not any(entry.name == 'Import:' + module.name for entry in _REGISTRY):
                _REGISTRY.append(Check('Import:' + module.name, ('Ia64', 'Amd64'),
                                       lambda context, message=message: [message]))
    return sorted(_REGISTRY, key=lambda c: c.name)
