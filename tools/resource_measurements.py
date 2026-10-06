"""Validate the instrumented T21 protocol; these reports never qualify budgets."""
import math
import re

PHASES = ('setupUs', 'hotReadUs', 'coldReadUs', 'novaTraversalUs', 'trimUs',
          'maintenanceUs', 'checkpointUs', 'closeUs', 'openUs')
PHASE_SAMPLES = {phase: 100 if phase in ('hotReadUs', 'coldReadUs', 'novaTraversalUs')
                 else 3 if phase == 'maintenanceUs' else 1 for phase in PHASES}
METRIC_SAMPLES = {**PHASE_SAMPLES, 'sqliteTraversalUs': 100, 'memoryTraversalUs': 100}
PHASE_SAMPLES_V2 = {**PHASE_SAMPLES, 'singleMutationUs': 100, 'batchMutationUs': 100}
METRIC_SAMPLES_V2 = {**METRIC_SAMPLES, 'singleMutationUs': 100, 'batchMutationUs': 100}


def mutation_protocol(samples=100, warmup=20):
    return {'records': 250, 'nodePropertyCount': 1, 'scalarType': 'int64',
            'operation': 'upsert-existing-node', 'durability': 'wal-sync',
            'database': 'dedicated', 'samples': samples, 'warmup': warmup,
            'validation': 'each-commit-and-reopen'}


def validate_mutation_protocol(run, samples=100, warmup=20):
    if integer(run.get('protocolVersion')) != 2 or integer(run.get('mutationBatchSize')) != 250:
        raise ValueError('unexpected native mutation protocol')
    protocol = mapping(run.get('mutationProtocol'))
    if protocol != mutation_protocol(samples, warmup):
        raise ValueError('mutation workload shape differs from protocol')
    for key in ('records', 'nodePropertyCount', 'samples', 'warmup'):
        integer(protocol[key])
    validation = mapping(run.get('mutationValidation'))
    expected = {'singleCommits': samples + warmup, 'batchCommits': samples + warmup,
                'mutations': (samples + warmup) * 251, 'reopenedRecords': 250, 'reopenVerified': True}
    if validation != expected or validation.get('reopenVerified') is not True:
        raise ValueError('mutation outcome/reopen validation differs from protocol')
    for key in ('singleCommits', 'batchCommits', 'mutations', 'reopenedRecords'):
        integer(validation[key])
STAGES = ('after-trim', 'maintenance-0', 'maintenance-1', 'maintenance-2',
          'final-checkpoint', 'after-close', 'after-reopen')
WORKLOADS = ('contacts', 'messages', 'knowledge', 'navigation', 'recommendations')
PROFILE_NODES = {'watch': 250, 'phone': 1000, 'desktop': 10000}


def integer(value):
    if type(value) is not int or value < 0:
        raise ValueError('resource counter must be a nonnegative integer')
    return value


def mapping(value):
    if not isinstance(value, dict):
        raise ValueError('resource evidence must contain objects')
    return value


def sequence(value):
    if not isinstance(value, list):
        raise ValueError('resource evidence must contain arrays')
    return value


def nonempty(value):
    return isinstance(value, str) and bool(value.strip())


def validate_resources(resource):
    resource = mapping(resource)
    version = integer(resource.get('schemaVersion'))
    if (version not in (1, 2) or resource.get('instrumented') is not True
            or not nonempty(resource.get('coverage'))):
        raise ValueError('invalid resource schema/coverage')
    phase_samples = PHASE_SAMPLES_V2 if version == 2 else PHASE_SAMPLES
    phases = mapping(resource.get('phases'))
    if set(phases) != set(phase_samples):
        raise ValueError('missing resource phases')
    allocated_peaks, reserved_peaks = [], []
    for name, phase in phases.items():
        phase = mapping(phase)
        if integer(phase.get('samples')) != phase_samples[name]:
            raise ValueError('resource phase sample count differs from protocol')
        allocation, workspace = mapping(phase.get('allocation')), mapping(phase.get('workspace'))
        for key in ('calls', 'totalBytes', 'peakBytes', 'maxRetainedBytes'):
            integer(allocation.get(key))
        if not allocation['totalBytes'] >= allocation['peakBytes'] >= allocation['maxRetainedBytes']:
            raise ValueError('inconsistent allocation peak')
        if allocation['totalBytes'] and (not allocation['calls'] or not allocation['peakBytes']):
            raise ValueError('allocated bytes require successful calls and a positive peak')
        for key in ('requests', 'maxPeakBytes', 'maxCurrentBytes', 'maxLimitBytes'):
            integer(workspace.get(key))
        # These harness contexts keep their effective ceiling for their lifetime.
        # General QueryContext peaks may exceed a limit tightened after release.
        if not workspace['maxLimitBytes'] >= workspace['maxPeakBytes'] >= workspace['maxCurrentBytes']:
            raise ValueError('inconsistent reservation peak')
        if workspace['requests']:
            if not 0 < workspace['maxLimitBytes'] <= 16 * 1024 * 1024:
                raise ValueError('invalid observed workspace ceiling')
            reserved_peaks.append(workspace['maxPeakBytes'])
        elif any(workspace[key] for key in ('maxPeakBytes', 'maxCurrentBytes', 'maxLimitBytes')):
            raise ValueError('unobserved workspace cannot have usage')
        allocated_peaks.append(allocation['peakBytes'])
    probe_outcomes = {}
    if version == 2:
        probes = mapping(resource.get('workspaceProbes'))
        if set(probes) != {'traversal', 'checkpoint'}:
            raise ValueError('missing workspace probes')
        for name, probe in probes.items():
            probe = mapping(probe)
            if probe.get('api') != 'execute_dsl' or probe.get('status') not in ('observed', 'limit-exceeded'):
                raise ValueError('unknown workspace probe outcome')
            workspace = mapping(probe.get('workspace'))
            for key in ('requests', 'maxPeakBytes', 'maxCurrentBytes', 'maxLimitBytes'):
                integer(workspace.get(key))
            if (workspace['requests'] != 1 or not 0 < workspace['maxLimitBytes'] <= 16 * 1024 * 1024
                    or not workspace['maxLimitBytes'] >= workspace['maxPeakBytes'] >= workspace['maxCurrentBytes']):
                raise ValueError('invalid workspace probe reservation')
            probe_outcomes[name] = probe['status']
    snapshots = sequence(resource.get('diskSnapshots'))
    if [mapping(snapshot).get('stage') for snapshot in snapshots] != list(STAGES):
        raise ValueError('missing disk lifecycle boundary')
    for snapshot in snapshots:
        categories = mapping(snapshot.get('categories'))
        if set(categories) != {'payload', 'catalog', 'wal', 'other'}:
            raise ValueError('missing disk categories')
        for key in ('logicalBytes', 'files'):
            if integer(snapshot.get(key)) != sum(integer(mapping(c).get(key)) for c in categories.values()):
                raise ValueError('disk totals differ from categories')
        if snapshot.get('allocationSupport') == 'unavailable':
            if (snapshot.get('allocatedBytes', 'absent') is not None
                    or any(c.get('allocatedBytes', 'absent') is not None for c in categories.values())):
                raise ValueError('unavailable disk allocation must be null')
        elif snapshot.get('allocationSupport') == 'st_blocks_512':
            if integer(snapshot.get('allocatedBytes')) != sum(integer(c.get('allocatedBytes')) for c in categories.values()):
                raise ValueError('disk allocation totals differ')
        else:
            raise ValueError('unknown disk allocation support')
        if any(c['files'] == 0 and (c['logicalBytes'] or c['allocatedBytes']) for c in categories.values()):
            raise ValueError('disk bytes require observed regular files')
    return {
        'maxTrackedPeakBytes': max(allocated_peaks),
        'maxReservedPeakBytes': max(reserved_peaks) if reserved_peaks else None,
        'maxLogicalBytes': max(s['logicalBytes'] for s in snapshots),
        'maxAllocatedBytes': None if any(s['allocatedBytes'] is None for s in snapshots)
        else max(s['allocatedBytes'] for s in snapshots),
        'maxFiles': max(s['files'] for s in snapshots),
        **({'workspaceProbeOutcomes': probe_outcomes} if version == 2 else {}),
    }


def validate_raw_metrics(run):
    version = integer(mapping(run.get('resources')).get('schemaVersion'))
    metric_samples = METRIC_SAMPLES_V2 if version == 2 else METRIC_SAMPLES
    phase_samples = PHASE_SAMPLES_V2 if version == 2 else PHASE_SAMPLES
    metrics, calls = mapping(run.get('metrics')), mapping(run.get('cppNewCalls'))
    if set(metrics) != set(metric_samples) or set(calls) != set(metric_samples):
        raise ValueError('unexpected raw resource metric inventory')
    for name, count in metric_samples.items():
        timings, allocations = sequence(metrics[name]), sequence(calls[name])
        if len(timings) != count or len(allocations) != count:
            raise ValueError('raw resource samples differ from protocol')
        for value in timings:
            if type(value) not in (int, float) or value < 0 or value > 1.7976931348623157e308 or not math.isfinite(value):
                raise ValueError('invalid resource timing sample')
        for value in allocations:
            integer(value)
        if name in phase_samples and sum(allocations) != run['resources']['phases'][name]['allocation']['calls']:
            raise ValueError('raw allocation calls differ from resource phase')


def summarize(report):
    report = mapping(report)
    if (integer(report.get('schemaVersion')) != 1 or report.get('complete') is not True
            or report.get('runFailures') != [] or type(report.get('dirty')) is not bool):
        raise ValueError('incomplete resource evidence')
    for key, length in (('revision', 40), ('sourceDigest', 64)):
        if not isinstance(report.get(key), str) or not re.fullmatch('[0-9a-f]{' + str(length) + '}', report[key]):
            raise ValueError('invalid resource provenance')
    profile = report.get('profile')
    if not isinstance(profile, str) or profile not in PROFILE_NODES:
        raise ValueError('unexpected resource profile')
    protocol = mapping(report.get('protocol'))
    expected = {'mode': 'resources', 'nodes': PROFILE_NODES[profile], 'runs': 1,
                'samples': 100, 'warmup': 20, 'seed': 42}
    version = integer(protocol.get('version', 1))
    if version == 2:
        expected.update(version=2, mutationBatchSize=250)
    elif version != 1:
        raise ValueError('unknown resource protocol version')
    if protocol != expected:
        raise ValueError('unexpected resource protocol')
    for key in ('nodes', 'runs', 'samples', 'warmup', 'seed'):
        integer(protocol[key])
    if version == 2:
        integer(protocol['mutationBatchSize'])
    environment = mapping(report.get('environment'))
    if environment.get('platform') not in ('macos', 'ios-simulator'):
        raise ValueError('unsupported resource environment')
    if any(not nonempty(environment.get(key)) for key in ('hardware', 'os', 'architecture', 'toolchain')):
        raise ValueError('missing resource environment')
    if environment['platform'] == 'ios-simulator':
        simulator = mapping(environment.get('simulator'))
        if any(not nonempty(simulator.get(key)) for key in ('runtime', 'device')):
            raise ValueError('missing resource simulator identity')
    workloads = sequence(report.get('workloads'))
    names = [mapping(work).get('name') for work in workloads]
    if len(names) != len(WORKLOADS) or any(names.count(name) != 1 for name in WORKLOADS):
        raise ValueError('missing resource workloads')
    tables = []
    for work in workloads:
        runs = sequence(work.get('runs'))
        if len(runs) != 1:
            raise ValueError('unexpected resource run count')
        run = mapping(runs[0])
        dataset = mapping(run.get('dataset'))
        if (run.get('platform') != environment['platform'] or dataset.get('name') != work['name']
                or integer(dataset.get('nodes')) != protocol['nodes'] or integer(dataset.get('seed')) != 42):
            raise ValueError('resource run provenance conflict')
        expected_shape = {'nodeStringPayloadBytes': 1024 if work['name'] == 'knowledge'
                          else 512 if work['name'] == 'messages' else 64,
                          'nodePropertyCount': 2, 'edgePropertyCount': 0}
        if integer(dataset.get('edges')) == 0 or any(integer(dataset.get(key)) != value for key, value in expected_shape.items()):
            raise ValueError('resource dataset shape differs from protocol')
        if integer(mapping(run.get('resources')).get('schemaVersion')) != version:
            raise ValueError('resource phase schema differs from collection protocol')
        if version == 2:
            validate_mutation_protocol(run)
        elif run.get('protocolVersion', 1) != 1 or 'mutationBatchSize' in run or 'mutationProtocol' in run:
            raise ValueError('mutation protocol cannot be presented as historical evidence')
        row = validate_resources(run.get('resources'))
        validate_raw_metrics(run)
        tables.append({'name': work['name'], **row})
    # Dirty runs remain inspectable; neither clean nor dirty instrumentation is a timing baseline.
    return {'status': 'diagnostic-only', 'hardwareQualified': False, 'failures': [], 'tables': tables}
