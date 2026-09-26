function control_tcp_matlab(port, gain, maxClients)
% Pure v1 velocity feedback over loopback TCP, MATLAB R2022a reference.
% Start before the solver/bridge. Each connection is one solver process.
% Example: control_tcp_matlab(45678, 2.0, 2) for a run and restart.
% No physical output; candidate state lives in the request, not this server.
arguments
    port (1,1) double {mustBeInteger,mustBePositive}
    gain (1,1) double {mustBeFinite,mustBeNonnegative}
    maxClients (1,1) double {mustBeInteger,mustBePositive} = 1
end
assert(port <= 65535 && maxClients <= 1000, 'Invalid port or client count');
sourceFile = fopen([mfilename('fullpath'), '.m'], 'rb');
assert(sourceFile >= 0, 'Cannot read controller source');
sourceBytes = fread(sourceFile, Inf, '*uint8'); fclose(sourceFile);
digest = java.security.MessageDigest.getInstance('SHA-256');
digest.update([sourceBytes; uint8(10); uint8(sprintf('%.17g', gain)).']);
endpointSha256 = lower(reshape(dec2hex(typecast(digest.digest(), 'uint8'), 2).', 1, []));
server = java.net.ServerSocket(port, 1, java.net.InetAddress.getByName('127.0.0.1'));
cleanupServer = onCleanup(@() server.close()); %#ok<NASGU>
server.setSoTimeout(120000);
fprintf('Range FEA MATLAB TCP controller ready on 127.0.0.1:%d\n', port);
for clientIndex = 1:maxClients
    client = server.accept();
    cleanupClient = onCleanup(@() client.close());
    reader = java.io.BufferedReader(java.io.InputStreamReader(client.getInputStream(), 'UTF-8'));
    writer = java.io.PrintWriter(java.io.OutputStreamWriter(client.getOutputStream(), 'UTF-8'), true);
    identity = []; sensorId = ''; actuatorId = '';
    while true
        % Bounded character reader avoids BufferedReader.readLine unbounded allocation.
        % FE assembly/solve may take arbitrarily long between accepted samples.
        % Only a partially received request has a 30-second completion deadline.
        client.setSoTimeout(0);
        c = reader.read();
        requestStart = tic;
        chars = zeros(1, 1048576, 'uint16'); count = 0;
        while true
            if c == -1 || c == 10, break; end
            count = count + 1;
            assert(count <= numel(chars), 'Protocol line exceeds limit');
            chars(count) = c;
            remainingMilliseconds = ceil(1000*(30-toc(requestStart)));
            assert(remainingMilliseconds > 0, 'Request receive deadline exceeded');
            client.setSoTimeout(remainingMilliseconds);
            c = reader.read();
        end
        if c == -1
            assert(count == 0, 'Truncated request');
            break;
        end
        request = jsondecode(char(chars(1:count)));
        assert(isstruct(request) && isscalar(request) && request.protocolVersion == 1, 'Unsupported protocol');
        assert(ischar(request.requestId) && ~isempty(request.requestId), 'Invalid request ID');
        response = struct('protocolVersion', 1, 'requestId', request.requestId, 'op', request.op);
        if strcmp(request.op, 'handshake')
            assert(isempty(identity), 'Duplicate handshake');
            caps = request.capabilities;
            assert(isequal(caps.pureStep, true) && strcmp(caps.controlMode, 'scalar-nodal-force'), 'Unsupported mode');
            assert(isstruct(caps.sensors) && numel(caps.sensors) <= 4096, 'Invalid sensor catalog');
            velocitySensor = find(strcmp({caps.sensors.unit}, 'm/s'));
            assert(numel(velocitySensor) == 1, 'One velocity sensor required');
            assert(numel(unique({caps.sensors.sensorId})) == numel(caps.sensors), 'Duplicate sensor ID');
            assert(iscell(caps.actuatorIds) && numel(caps.actuatorIds) == 1, 'One force actuator required');
            sensorId = caps.sensors(velocitySensor).sensorId; actuatorId = caps.actuatorIds{1};
            identity = request;
            response.ok = true;
            response.endpointSha256 = endpointSha256;
            response.capabilities = struct('pureStep', true, 'controlMode', 'scalar-nodal-force', 'actuatorIds', {caps.actuatorIds});
            for field = {'controllerId','controllerRevision','modelSnapshotSha256','deterministicSeed'}
                response.(field{1}) = request.(field{1});
            end
        elseif strcmp(request.op, 'compute')
            assert(~isempty(identity), 'Handshake required');
            observations = request.observations;
            assert(isstruct(observations) && numel(observations) == numel(identity.capabilities.sensors), 'Observation count mismatch');
            assert(numel(unique({observations.sensorId})) == numel(observations), 'Duplicate observation');
            for observationIndex = 1:numel(observations)
                entry = observations(observationIndex);
                catalogIndex = find(strcmp({identity.capabilities.sensors.sensorId}, entry.sensorId));
                assert(numel(catalogIndex) == 1 && strcmp(entry.unit, identity.capabilities.sensors(catalogIndex).unit), 'Observation catalog mismatch');
                assert(isnumeric(entry.value) && isscalar(entry.value) && isfinite(entry.value), 'Invalid observation');
            end
            obs = observations(strcmp({observations.sensorId}, sensorId));
            assert(isnumeric(obs.value) && isscalar(obs.value) && isfinite(obs.value), 'Invalid observation');
            assert(isnumeric(request.dt) && isscalar(request.dt) && isfinite(request.dt) && request.dt > 0, 'Invalid dt');
            state = request.acceptedControllerState;
            assert(isstruct(state) && isscalar(state), 'Invalid accepted state');
            sampleCount = 0;
            if isfield(state, 'sampleCount'), sampleCount = state.sampleCount; end
            assert(isnumeric(sampleCount) && isscalar(sampleCount) && isfinite(sampleCount) && sampleCount >= 0 && sampleCount == floor(sampleCount) && sampleCount < 2^53-1, 'Invalid sample count');
            force = -gain * obs.value;
            assert(isfinite(force), 'Non-finite command');
            response.stepId = request.stepId;
            response.commands = {struct('actuatorId', actuatorId, 'force', force, 'unit', 'N')};
            response.nextControllerState = struct('sampleCount', sampleCount+1);
        else
            error('Unknown operation');
        end
        writer.println(jsonencode(response));
        assert(~writer.checkError(), 'Socket output failed');
    end
    clear cleanupClient;
end
end
