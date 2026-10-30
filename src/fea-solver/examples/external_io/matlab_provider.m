function matlab_provider(port)
% MATLAB TCP reference provider for range-external-io/1.
% Start with: matlab -batch "matlab_provider(6011)"
% The host owns FE time, accepted state, voltage limits, and commit/rollback.
if nargin ~= 1 || ~isscalar(port) || port ~= fix(port) || port < 1 || port > 65535
    error('port must be an integer from 1 to 65535');
end
server = javaObject('java.net.ServerSocket');
server.setReuseAddress(true);
address = javaObject('java.net.InetSocketAddress', '127.0.0.1', port);
server.bind(address);
cleanup = onCleanup(@() server.close()); %#ok<NASGU>
fprintf(2, 'Listening on 127.0.0.1:%d\n', port);

while true
    socket = server.accept();
    socket.setTcpNoDelay(true);
    clientCleanup = onCleanup(@() socket.close()); %#ok<NASGU>
    reader = javaObject('java.io.BufferedReader', ...
        javaObject('java.io.InputStreamReader', socket.getInputStream(), 'UTF-8'));
    writer = javaObject('java.io.BufferedWriter', ...
        javaObject('java.io.OutputStreamWriter', socket.getOutputStream(), 'UTF-8'));
    runId = '';
    ports = {};
    roles = {};
    previousRequest = '';
    ready = false;
    try
        while true
            wireLine = reader.readLine();
            if isempty(wireLine)
                break;
            end
            request = jsondecode(char(wireLine));
            validateEnvelope(request, previousRequest);
            previousRequest = request.requestId;
            switch request.op
                case 'handshake'
                    if ready || ~strcmp(request.protocol, 'range-external-io/1') || ...
                            ~isequal(request.algorithm, struct('id','range-example-matlab-provider','version','1'))
                        error('unsupported or repeated handshake');
                    end
                    if isempty(request.ports)
                        error('handshake requires at least one physical port');
                    end
                    ports = asCell(request.ports);
                    roles = unique(cellfun(@(p) p.role, ports, 'UniformOutput', false));
                    runId = request.runId;
                    initialState = struct();
                    if any(strcmp(roles, 'excitation'))
                        initialState.excitation = struct('samples', '0');
                    end
                    if any(strcmp(roles, 'controller'))
                        initialState.controller = struct('integral', 0.0);
                    end
                    response = envelope(request);
                    response.modelDigest = request.modelDigest;
                    response.configDigest = request.configDigest;
                    response.mappingDigest = request.mappingDigest;
                    response.algorithm = request.algorithm;
                    response.ports = ports; % cell array preserves JSON array shape for one port
                    response.capabilities = struct('explicitState', true, 'futurePreview', false);
                    response.initialState = initialState;
                    ready = true;
                case 'evaluate'
                    if ~ready || ~strcmp(request.runId, runId) || ~any(strcmp(roles, request.role))
                        error('handshake, run identity, or role mismatch');
                    end
                    response = evaluateRequest(request, ports);
                case 'close'
                    if ~ready || ~strcmp(request.runId, runId)
                        error('close before handshake or run identity mismatch');
                    end
                    response = envelope(request);
                otherwise
                    error('unknown operation');
            end
            encoded = jsonencode(response);
            if isempty(encoded)
                error('failed to encode protocol response');
            end
            writer.write(javaObject('java.lang.String', [encoded char(10)]));
            writer.flush();
            if strcmp(request.op, 'close')
                break;
            end
        end
    catch failure
        fprintf(2, 'External I/O session failed: %s\n', failure.message);
        rethrow(failure);
    end
    clear clientCleanup;
end
end

function validateEnvelope(request, previousRequest)
required = {'protocol','op','runId','requestId'};
for k = 1:numel(required)
    if ~isfield(request, required{k}) || ~ischar(request.(required{k}))
        error('invalid request envelope');
    end
end
if ~strcmp(request.protocol, 'range-external-io/1') || ...
        isempty(regexp(request.requestId, '^\d+$', 'once'))
    error('invalid protocol or request ID');
end
if ~isempty(previousRequest) && ~decimalGreater(request.requestId, previousRequest)
    error('request IDs must increase');
end
end

function yes = decimalGreater(a, b)
a = regexprep(a, '^0+(?=\d)', '');
b = regexprep(b, '^0+(?=\d)', '');
yes = numel(a) > numel(b) || (numel(a) == numel(b) && lexGreater(a, b));
end

function yes = lexGreater(a, b)
yes = false;
for k = 1:numel(a)
    if a(k) ~= b(k)
        yes = a(k) > b(k);
        return;
    end
end
end

function response = envelope(request)
response = struct('protocol', request.protocol, 'op', request.op, ...
    'runId', request.runId, 'requestId', request.requestId);
end

function response = evaluateRequest(request, ports)
if ~any(strcmp(request.role, {'excitation','controller'})) || ...
        ~isfinite(request.time) || ...
        ~isfinite(request.samplePeriod) || request.samplePeriod <= 0 || ...
        ~isfinite(request.validUntil) || request.validUntil <= request.time
    error('invalid role or simulation interval');
end
outputs = {};
measurement = 0.0;
if strcmp(request.role, 'controller')
    if isempty(request.inputs)
        error('this example controller requires velocity input');
    end
    inputs = asCell(request.inputs);
    for k = 1:numel(inputs)
        input = inputs{k};
        if ~strcmp(input.unit, 'm/s') || ~isfinite(input.value)
            error('this example controller accepts finite velocity inputs in m/s');
        end
        measurement = measurement + input.value;
    end
end
index = 0;
for k = 1:numel(ports)
    port = ports{k};
    if ~strcmp(port.role, request.role) || ~strcmp(port.direction, 'output')
        continue;
    end
    index = index + 1;
    if strcmp(request.role, 'excitation')
        if ~any(strcmp(port.quantity, {'force','moment'}))
            error('excitation outputs must be force or moment');
        end
        value = 0.05 * index * sin(2*pi*index*request.time);
    else
        if ~strcmp(port.quantity, 'voltage') || ~strcmp(port.unit, 'V') || ...
                ~strcmp(port.reference, 'absolute')
            error('controller outputs must be absolute voltage in V');
        end
        state = request.state;
        if ~isfield(state, 'integral') || ~isfinite(state.integral)
            error('controller state requires a finite integral');
        end
        integral = state.integral + request.samplePeriod * measurement;
        value = -2.0 * measurement - 0.1 * integral;
    end
    outputs{end+1} = struct('id',port.id,'unit',port.unit, ...
        'reference',port.reference,'value',value); %#ok<AGROW>
end
response = envelope(request);
response.step = request.step;
response.tick = request.tick;
response.acceptedStateDigest = request.acceptedStateDigest;
response.outputs = outputs;
if strcmp(request.role, 'excitation')
    sampleCount = str2double(request.state.samples);
    if ~isfinite(sampleCount) || sampleCount < 0 || sampleCount ~= fix(sampleCount)
        error('invalid excitation sample counter');
    end
    response.candidateState = struct('samples', sprintf('%.0f', sampleCount + 1));
else
    response.candidateState = struct('integral', integral);
end
end

function values = asCell(value)
if iscell(value)
    values = value;
elseif isstruct(value)
    values = arrayfun(@(item) item, value, 'UniformOutput', false);
else
    values = {};
end
end
