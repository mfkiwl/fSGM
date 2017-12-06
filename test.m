close all;

seqIndex = 4;

I0 = imread(sprintf('%06d_10.png', seqIndex));
I1 = imread(sprintf('%06d_11.png', seqIndex));

% F = estimate_Fmatrix(I0, I1);

% P0 = read_calib_file(sprintf('%06d.txt', seqIndex));
% 
% 
% [w, t, E1, e1, e2, F] = mono_vo(I0, I1, [P0(1,1) P0(2,2)], [P0(1, 3) P0(2, 3)]);
% 
% flow = rotation_motion(w, P0(1,1), [P0(1, 3) P0(2, 3)], [size(I0, 2), size(I0, 1)]);

% Fc = flow_to_color(flow);
% imshow(Fc);

% flow_field(flow, I0);

load workspace2.mat
% 
% C = calc_cost(I0, I1, e2, flow);
% 
% save('workspace.mat', 'C', 'e1', 'e2', 'flow', 'w', 't', 'E1', 'F');

bestD = sgm(C);
% 
rows = size(I0, 1);
cols = size(I0, 2);
P = zeros(rows, cols, 2);
[P(:,:,1), P(:,:,2)] = meshgrid(1:cols, 1:rows);
E2I = zeros(rows, cols, 2);
e2i = [e2(1)/e2(3); e2(2)/e2(3)];
E2I(:,:,1) = repmat(e2i(1), rows, cols);
E2I(:,:,2) = repmat(e2i(2), rows, cols);
normlizeDirection = normlize(P + flow - E2I);
flowT = (bestD-1).*normlizeDirection;

flowFinal = flowT + flow;
flowFinal(:,:,3) = 1;

gtFlow = flow_read_kitti('000004_10_gtflow.png');


eimage  =  flow_error_image(gtFlow, flowFinal, [3; 0.05]);
figure;
imshow(eimage);
[outlier, aepe] = flow_error(gtFlow, flowFinal, [3;0.05]);
title(['outlier percentage: ', num2str(outlier), ' AEPE: ', num2str(aepe)]);

function dnorm = normlize(d)
    if(size(d, 3) > 1)
        dnorm = d./sqrt(sum(d.^2, 3));
    else
        dnorm = d/sqrt(sum(d.^2));
    end
end
